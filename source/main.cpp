#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <curl/curl.h>
#include <json-c/json.h>

#include <algorithm>
#include <cfloat>
#include <cinttypes>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <malloc.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <cerrno>
#include <atomic>
#include <cstddef>

#include "imgui/imgui.h"
#include "imgui/imgui_sw.h"

#include <3ds/ndsp/ndsp.h>
#include <tremor/ivorbisfile.h>

/* -------------------------------------------------------------------------- */
/*  Constants                                                                 */
/* -------------------------------------------------------------------------- */

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000
static u32 *socBuffer = nullptr;

static constexpr const char *USER_AGENT        = "CTGP-7-Mod-Manager/3.0";
static constexpr const char *API_V10_INDEX     = "https://gamebanana.com/apiv10/Mod/Index";
static constexpr const char *API_CORE_DATA     = "https://api.gamebanana.com/Core/Item/Data";
static constexpr const char *CA_BUNDLE_PATH    = "romfs:/cacert.pem";

static constexpr size_t MAX_RESPONSE_SIZE       = 512 * 1024;
static constexpr int    MAX_FETCH_ATTEMPTS      = 4;
static constexpr u64    RETRY_BASE_DELAY_NS     = 500000000ULL;
/* Only used when the server actually pushes back (429/503); there are no unconditional
 * delays anywhere in the fetch path. */
static constexpr u64    THROTTLE_DELAY_NS       = 1500000000ULL;

/* Requests run on a small pool of worker threads, each with its own easy handle. Six
 * keeps the ARM11 busy through the TLS handshakes without exhausting sockets. */
static constexpr size_t FETCH_WORKERS      = 6;
static constexpr size_t WORKER_STACK_SIZE  = 128 * 1024;
static constexpr int    WORKER_PRIORITY    = 0x3F;   /* below the UI thread */

static constexpr int    INDEX_PER_PAGE     = 50;     /* apiv10 page size cap */
static constexpr int    MAX_PAGES_PER_CAT  = 200;    /* runaway guard on bad metadata */
static constexpr size_t CORE_BATCH_SIZE    = 50;     /* mods per Core/Item/Data request */
static constexpr size_t RESPONSE_RESERVE   = 64 * 1024;

static constexpr int CATEGORIES[] = {
    35931, 10605, 35932, 35943, 35933, 35935, 35937, 35938,
    35939, 35941, 35942, 35944, 35946, 35947, 35945, 35940,
    35934, 35936
};
static constexpr size_t NUM_CATEGORIES = sizeof(CATEGORIES) / sizeof(CATEGORIES[0]);

#define BASE_DIR               "sdmc:/"
#define APP_DIR                BASE_DIR "3ds/CTGP-7-Mod-Manager/"
#define CACHE_DIR              APP_DIR "cache/"
#define LISTS_DIR              CACHE_DIR "lists/"
#define THUMBNAIL_CACHE_DIR    CACHE_DIR "images/"
#define CTGP7_DIR              BASE_DIR "CTGP-7/MyStuff/Characters/"

#define MOD_LIST_FILE   LISTS_DIR "modlist.json"
#define BY_NAME_FILE    LISTS_DIR "byname.json"
#define BY_UPDATED_FILE LISTS_DIR "byupdated.json"

/* Lives outside CACHE_DIR: the fetcher wipes the cache on every launch, but the
 * record of what the user has installed has to survive that. */
#define INSTALLED_FILE  APP_DIR "installed_mods.json"

/* -------------------------------------------------------------------------- */
/*  Audio (libvorbisidec + NDSP)                                              */
/* -------------------------------------------------------------------------- */

static constexpr int    AUDIO_BUF_SAMPLES  = 4096;
static constexpr int    AUDIO_MAX_CHANNELS = 1;
static constexpr int    AUDIO_VOL          = 0x50;

static OggVorbis_File g_ov{};
static bool           g_loopMode = false;
static std::atomic<bool> g_audioShouldStop{false};
static int            g_channels = 1;
static int            g_rate     = 32768;
static ndspWaveBuf    g_wavebuf[2];
static Thread         g_audioThread = nullptr;
static bool           g_ndspReady   = false;

/* -------------------------------------------------------------------------- */
/*  Color helpers (easy to set individual drawing text / UI colors)            */
/* -------------------------------------------------------------------------- */

static ImVec4 MakeTextColor(u8 r, u8 g, u8 b, u8 a = 255)
{
    return ImVec4(
        static_cast<float>(r) / 255.0f,
        static_cast<float>(g) / 255.0f,
        static_cast<float>(b) / 255.0f,
        static_cast<float>(a) / 255.0f
    );
}

/* Palette lifted verbatim from Infrastructure/Theme.cs and the .axaml views, so the
 * 3DS build reads identically to the desktop original. */
static const u32 CLR_BG     = C2D_Color32(0x15, 0x1D, 0x23, 0xFF); /* window + unselected card  */
static const u32 CLR_SEL_BG = C2D_Color32(0x2A, 0x3B, 0x47, 0xFF); /* Theme.SelBg               */
static const u32 CLR_GOLD   = C2D_Color32(0xAB, 0xA0, 0x22, 0xFF); /* accent / not installed    */
static const u32 CLR_GREEN  = C2D_Color32(0x4C, 0xAF, 0x50, 0xFF); /* installed                 */
static const u32 CLR_AMBER  = C2D_Color32(0xFF, 0xC1, 0x07, 0xFF); /* update available          */
static const u32 CLR_AUTHOR = C2D_Color32(0x88, 0x88, 0x88, 0xFF);
static const u32 CLR_ERROR  = C2D_Color32(0xFF, 0x55, 0x55, 0xFF);
static const u32 CLR_THUMB  = C2D_Color32(0x00, 0x00, 0x00, 0xFF); /* thumbnail placeholder     */

static const ImVec4 IM_GOLD      = MakeTextColor(0xAB, 0xA0, 0x22);
static const ImVec4 IM_AMBER     = MakeTextColor(0xFF, 0xC1, 0x07);
static const ImVec4 IM_AUTHOR    = MakeTextColor(0x88, 0x88, 0x88);
static const ImVec4 IM_ERROR     = MakeTextColor(0xFF, 0x55, 0x55);
static const ImVec4 IM_BTN_BG    = MakeTextColor(0x2A, 0x3B, 0x47);
static const ImVec4 IM_BTN_HOT   = MakeTextColor(0x38, 0x4E, 0x5D);
static const ImVec4 IM_UNINST_BG = MakeTextColor(0x4A, 0x27, 0x27);
static const ImVec4 IM_UNINST_HOT= MakeTextColor(0x63, 0x34, 0x34);
static const ImVec4 IM_UNINST_FG = MakeTextColor(0xFF, 0x88, 0x88);

/* -------------------------------------------------------------------------- */
/*  Screen layout                                                             */
/* -------------------------------------------------------------------------- */

static constexpr float TOP_W = 400.0f, TOP_H = 240.0f;
static constexpr float BOT_W = 320.0f, BOT_H = 240.0f;

/* The desktop original tiles 12 cards three-wide in a 400x240 portrait-ish window.
 * The 3DS top screen is the same 400x240 but landscape, so the same 12 cards go
 * four-wide: that keeps each card near the original's 120x105 proportions instead of
 * squashing it into a 133x60 letterbox with no room for a thumbnail. Paging still
 * advances exactly one row at a time, as it does upstream. */
static constexpr int GRID_COLS      = 4;
static constexpr int GRID_ROWS      = 3;
static constexpr int CARDS_PER_PAGE = GRID_COLS * GRID_ROWS;

static constexpr float CELL_W      = TOP_W / GRID_COLS;              /* 100 */
static constexpr float CELL_H      = TOP_H / GRID_ROWS;              /*  80 */
static constexpr float CARD_MARGIN = 2.0f;                           /* gap between cells       */
static constexpr float CARD_BORDER = 2.0f;                           /* BorderThickness="2"     */
static constexpr float CARD_W      = CELL_W - CARD_MARGIN * 2.0f;    /*  96 */
static constexpr float CARD_H      = CELL_H - CARD_MARGIN * 2.0f;    /*  76 */
static constexpr float CONTENT_W   = CARD_W - CARD_BORDER * 2.0f;    /*  92 */
static constexpr float CONTENT_H   = CARD_H - CARD_BORDER * 2.0f;    /*  72 */
static constexpr float TEXT_MAX_W  = CONTENT_W - 4.0f;               /*  88 */

/* Row offsets are measured from the card's content origin and stack to CONTENT_H. */
static constexpr float THUMB_H   = 38.0f;
static constexpr float NAME_Y    = 39.0f, NAME_PX   = 12.0f;  /* original FontSize 10 */
static constexpr float AUTHOR_Y  = 51.0f, AUTHOR_PX =  9.6f;  /* original FontSize  8 */
static constexpr float STATUS_Y  = 61.0f, STATUS_PX = 10.2f;  /* original FontSize 10 */
static constexpr float MSG_PX    = 18.0f;                     /* full-screen messages */

/* Bottom-screen controls, sized as in BottomScreenWindow.axaml. */
static constexpr float BTN_W        = 280.0f;
static constexpr float BTN_X        = (BOT_W - BTN_W) * 0.5f;
static constexpr float ACTION_BTN_H = 44.0f;
static constexpr float UNINST_BTN_H = 34.0f;
static constexpr float SORT_LABEL_Y =   6.0f;
static constexpr float SORT_ROW_Y   =  26.0f;
static constexpr float SEP_Y        =  54.0f;
static constexpr float ACTION_BTN_Y =  62.0f;
static constexpr float UNINST_BTN_Y = 114.0f;
static constexpr float COUNTER_Y    = 158.0f;
static constexpr float HINT1_Y      = 190.0f;
static constexpr float HINT2_Y      = 210.0f;

/* The action label wraps instead of being cut to a fixed character count, so names like
 * "Mario Kart DS Character Pack (Wave 3)" show in full. */
static constexpr float BTN_TEXT_PAD = 8.0f;
static constexpr float BTN_ROUNDING = 5.0f;
static constexpr int   BTN_MAX_LINES = 2;   /* 2 x 16px fits inside the 44px button */

/* 12 cards x 3 lines, each capped near 24 glyphs by the width fitter. */
static constexpr size_t CARD_TEXT_GLYPHS = 2048;
static constexpr size_t SCRATCH_GLYPHS   = 512;
static constexpr size_t MEASURE_MAX_LEN  = 192;   /* keeps the scratch buffer bounded */

/* Held-direction auto-repeat, standing in for the OS key repeat the desktop build gets. */
static constexpr float NAV_REPEAT_DELAY = 0.35f;
static constexpr float NAV_REPEAT_RATE  = 0.07f;

static constexpr ImGuiWindowFlags SCREEN_WINDOW_FLAGS =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

struct ModData {
    int         Id             = 0;
    std::string Name;
    std::string Author;
    std::string ThumbnailUrl;
    std::string LatestFileUrl;
    int64_t     LatestFileDate = 0;
    std::string LatestFileName;
};

/* Mirrors Core.Models.InstallRecord. `Date` is the mod's LatestFileDate at the moment
 * it was installed, which is what makes "newer file upstream" detectable later. */
struct InstallRecord {
    int64_t                  Date = 0;
    std::vector<std::string> Files;
    std::string              SourceFileName;
};

struct FetchResult {
    std::string data;
    long        response_code = 0;
    bool        success       = false;
};

/* STATE_LOADING exists so the status screen can paint "Loading mods..." for one frame
 * before the (blocking) list parse runs on the next. */
enum AppState  { STATE_FETCHING, STATE_LOADING, STATE_BROWSING, STATE_FAILED };
enum ModAction { ACTION_NONE, ACTION_INSTALL, ACTION_UPDATE, ACTION_INSTALLED };

static LightLock status_lock;
static std::string g_statusText = "Initializing...";
static bool g_fetchDone = false;

/* -------------------------------------------------------------------------- */
/*  Browser state (main thread only -- never touched by the fetch thread)      */
/* -------------------------------------------------------------------------- */

static AppState                     g_appState   = STATE_FETCHING;
static std::vector<ModData>         g_mods;                 /* full list, in sort order   */
static std::map<int, InstallRecord> g_installed;            /* keyed by mod id            */
static int                          g_winStart   = 0;       /* first visible card, row-aligned */
static int                          g_selIdx     = 0;       /* cursor within the window   */
static bool                         g_sortByName = false;   /* false == recently updated  */
static std::string                  g_errorText;

/* Card labels are parsed into citro2d text objects once per window change instead of
 * every frame; only the colours are recomputed per draw. */
struct CardText {
    C2D_Text name, author, status;
    bool     hasName = false, hasAuthor = false, hasStatus = false;
    float    nameW = 0.0f, authorW = 0.0f, statusW = 0.0f;
};

static C2D_TextBuf g_cardTextBuf   = nullptr;   /* persistent: holds the parsed card text */
static C2D_TextBuf g_scratchBuf    = nullptr;   /* transient: measuring and one-off text  */
static CardText    g_cardText[CARDS_PER_PAGE];
static int         g_cardTextCount = 0;
static bool        g_cardTextDirty = true;

/* Text scales derive from the shared system font's authored line height. */
static float g_fontLineH   = 30.0f;
static float g_nameScale   = 1.0f;
static float g_authorScale = 1.0f;
static float g_statusScale = 1.0f;
static float g_msgScale    = 1.0f;

static u32   g_navHeldKey = 0;
static float g_navTimer   = 0.0f;

/* -------------------------------------------------------------------------- */
/*  Logging (Thread-Safe)                                                     */
/* -------------------------------------------------------------------------- */

static void print_status(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (written < 0) {
        buffer[0] = '\0';
    } else if (static_cast<size_t>(written) >= sizeof(buffer)) {
        size_t len = sizeof(buffer) - 4;
        buffer[len]     = '.';
        buffer[len + 1] = '.';
        buffer[len + 2] = '.';
        buffer[len + 3] = '\0';
    }

    LightLock_Lock(&status_lock);
    g_statusText = buffer;
    LightLock_Unlock(&status_lock);
}

/* -------------------------------------------------------------------------- */
/*  Filesystem helpers                                                        */
/* -------------------------------------------------------------------------- */

static int rmrf(const char *path) {
    std::vector<std::string> stack;
    stack.push_back(path);

    while (!stack.empty()) {
        std::string current = std::move(stack.back());
        stack.pop_back();

        DIR *d = opendir(current.c_str());
        if (!d) {
            if (errno == ENOENT) continue;
            return -1;
        }

        struct dirent *entry;
        bool has_children = false;
        while ((entry = readdir(d)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            std::string fullpath = current;
            if (!fullpath.empty() && fullpath.back() != '/') fullpath += "/";
            fullpath += entry->d_name;

            struct stat st;
            if (lstat(fullpath.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
                    stack.push_back(current);
                    stack.push_back(fullpath);
                    has_children = true;
                    break;
                } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
                    if (unlink(fullpath.c_str()) != 0) { closedir(d); return -1; }
                } else {
                    unlink(fullpath.c_str());
                }
            } else if (errno != ENOENT) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);

        if (!has_children) {
            if (rmdir(current.c_str()) != 0 && errno != ENOENT) return -1;
        }
    }
    return 0;
}

static bool mkdir_p(const char *path) {
    std::string buf = path;
    const size_t prefix_len = std::string("sdmc:/").length();
    const size_t start_idx  = (buf.find("sdmc:/") == 0) ? prefix_len : 0;

    for (size_t i = start_idx; i < buf.length(); i++) {
        if (buf[i] == '/') {
            std::string sub_path = buf.substr(0, i);
            if (mkdir(sub_path.c_str(), 0777) != 0 && errno != EEXIST) {
                print_status("mkdir_p: failed to create ‘%s’", sub_path.c_str());
                return false;
            }
        }
    }
    if (mkdir(buf.c_str(), 0777) != 0 && errno != EEXIST) {
        print_status("mkdir_p: failed to create ‘%s’", path);
        return false;
    }
    return true;
}

static bool init_paths() {
    return  mkdir_p(APP_DIR) &&
            mkdir_p(LISTS_DIR) &&
            mkdir_p(THUMBNAIL_CACHE_DIR) &&
            mkdir_p(CTGP7_DIR);
}

/* -------------------------------------------------------------------------- */
/*  libcurl callbacks                                                         */
/* -------------------------------------------------------------------------- */

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    auto *result = static_cast<std::string*>(userp);

    if (total > 0 && result->size() > MAX_RESPONSE_SIZE - total) {
        return CURL_WRITEFUNC_ERROR;
    }

    if (total == 0) return 0;
    result->append(static_cast<char*>(contents), total);
    return total;
}

/* -------------------------------------------------------------------------- */
/*  HTTP / network                                                            */
/* -------------------------------------------------------------------------- */

/* The worker pool shares one DNS cache so a host is resolved once for the whole run.
 * TLS sessions are deliberately NOT shared: devkitPro's mbedTLS is built without
 * MBEDTLS_THREADING_C, so libcurl gives every connection its own entropy and DRBG
 * context (safe), but a session cache handed between threads is not worth the risk.
 * Each worker keeps its own connection alive instead, so it handshakes exactly once. */
static CURLSH   *g_curlShare = nullptr;
static LightLock g_shareLocks[CURL_LOCK_DATA_LAST];

static void share_lock_cb(CURL*, curl_lock_data data, curl_lock_access, void*) {
    if (data < CURL_LOCK_DATA_LAST) LightLock_Lock(&g_shareLocks[data]);
}

static void share_unlock_cb(CURL*, curl_lock_data data, void*) {
    if (data < CURL_LOCK_DATA_LAST) LightLock_Unlock(&g_shareLocks[data]);
}

static void share_init() {
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) LightLock_Init(&g_shareLocks[i]);

    g_curlShare = curl_share_init();
    if (!g_curlShare) return;   /* optional: the pool still works, just colder */

    curl_share_setopt(g_curlShare, CURLSHOPT_LOCKFUNC,   share_lock_cb);
    curl_share_setopt(g_curlShare, CURLSHOPT_UNLOCKFUNC, share_unlock_cb);
    curl_share_setopt(g_curlShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
}

static void share_cleanup() {
    if (g_curlShare) {
        curl_share_cleanup(g_curlShare);
        g_curlShare = nullptr;
    }
}

/* Everything that never varies between requests is set once per handle. Avoiding
 * curl_easy_reset() is what lets libcurl hold the connection (and its negotiated TLS
 * session) open across every request that handle makes. */
static void curl_configure(CURL *curl) {
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE_PATH);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   /* gzip: JSON shrinks ~8x */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (g_curlShare) curl_easy_setopt(curl, CURLOPT_SHARE, g_curlShare);
}

static std::atomic<int> g_failedRequests(0);

static FetchResult curl_get(CURL *curl, const std::string& url) {
    FetchResult result;
    if (!curl) return result;

    result.data.reserve(RESPONSE_RESERVE);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.data);

    const CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK &&
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.response_code) == CURLE_OK) {
        result.success = (result.response_code >= 200 && result.response_code < 300);
    }

    /* Workers run concurrently, so per-request chatter would just fight over the status
     * line; failures are counted and reported once the run finishes. */
    if (!result.success) {
        g_failedRequests.fetch_add(1);
        result.data.clear();
    }
    return result;
}

/* A 4xx that says the request itself is malformed or oversized; retrying the identical
 * URL cannot fix it, and for a batched request it means the batch was too big. */
static bool is_request_too_large(long code) {
    return code == 400 || code == 413 || code == 414;
}

static FetchResult curl_get_retry(CURL *curl, const std::string& url, int max_attempts) {
    FetchResult result;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (attempt > 0) svcSleepThread(RETRY_BASE_DELAY_NS << (attempt - 1));

        result = curl_get(curl, url);
        if (result.success) return result;
        if (is_request_too_large(result.response_code)) break;

        /* Rate limited or briefly unavailable: ease off before trying again. */
        if (result.response_code == 429 || result.response_code == 503) {
            svcSleepThread(THROTTLE_DELAY_NS);
        }
    }
    return result;   /* the last failure, so callers can inspect the status code */
}

/* -------------------------------------------------------------------------- */
/*  Worker pool                                                               */
/* -------------------------------------------------------------------------- */

typedef void (*JobFn)(CURL *curl, size_t job_index, void *user);

struct PoolCtx {
    std::atomic<size_t> next;
    size_t              count = 0;
    JobFn               fn    = nullptr;
    void               *user  = nullptr;
};

static void pool_worker(void *arg) {
    PoolCtx *ctx = static_cast<PoolCtx*>(arg);

    CURL *curl = curl_easy_init();
    if (!curl) return;
    curl_configure(curl);

    for (;;) {
        const size_t i = ctx->next.fetch_add(1);
        if (i >= ctx->count) break;
        ctx->fn(curl, i, ctx->user);
    }

    curl_easy_cleanup(curl);
}

/* Runs `fn` over [0, count) across the worker pool and returns once every job is done.
 * Falls back to running inline if no thread could be created. */
static void run_jobs(size_t count, JobFn fn, void *user) {
    if (count == 0) return;

    PoolCtx ctx;
    ctx.next.store(0);
    ctx.count = count;
    ctx.fn    = fn;
    ctx.user  = user;

    /* The first job runs here, on its own. That resolves the host into the shared DNS
     * cache and proves the endpoint before six workers would otherwise hit it at once
     * with six cold handshakes. It is a real job, so nothing is wasted. */
    CURL *warm = curl_easy_init();
    if (warm) {
        curl_configure(warm);
        const size_t i = ctx.next.fetch_add(1);
        if (i < ctx.count) ctx.fn(warm, i, ctx.user);
        curl_easy_cleanup(warm);
    }
    if (ctx.next.load() >= ctx.count) return;   /* that was the only job */

    const size_t remaining = ctx.count - ctx.next.load();
    const size_t want = remaining < FETCH_WORKERS ? remaining : FETCH_WORKERS;
    Thread threads[FETCH_WORKERS] = {};
    size_t started = 0;

    for (size_t i = 0; i < want; i++) {
        threads[started] = threadCreate(pool_worker, &ctx, WORKER_STACK_SIZE,
                                        WORKER_PRIORITY, -2, false);
        if (!threads[started]) break;
        started++;
    }

    if (started == 0) {
        pool_worker(&ctx);          /* no threads available: do it all here */
        return;
    }
    for (size_t i = 0; i < started; i++) {
        threadJoin(threads[i], U64_MAX);
        threadFree(threads[i]);
    }
}

/* Shared progress line. `g_phaseLabel` is set before a phase starts, so workers only
 * ever read it. */
static std::atomic<int> g_jobsDone(0);
static int              g_jobsTotal = 0;
static const char      *g_phaseLabel = "";

static void begin_phase(const char *label, size_t total) {
    g_phaseLabel = label;
    g_jobsTotal  = static_cast<int>(total);
    g_jobsDone.store(0);
    print_status("%s 0/%d...", label, g_jobsTotal);
}

static void report_progress() {
    const int done = g_jobsDone.fetch_add(1) + 1;
    print_status("%s %d/%d...", g_phaseLabel, done, g_jobsTotal);
}

/* -------------------------------------------------------------------------- */
/*  JSON extraction helpers                                                   */
/* -------------------------------------------------------------------------- */

static const char *get_json_string(json_object *obj, const char *key) {
    json_object *val;
    if (json_object_object_get_ex(obj, key, &val) &&
        json_object_get_type(val) == json_type_string) {
        return json_object_get_string(val);
    }
    return nullptr;
}

static int get_json_int(json_object *obj, const char *key) {
    json_object *val;
    if (json_object_object_get_ex(obj, key, &val)) {
        if (json_object_get_type(val) == json_type_int) {
            long long v = json_object_get_int64(val);
            if (v < INT_MIN || v > INT_MAX) return 0;
            return static_cast<int>(v);
        } else if (json_object_get_type(val) == json_type_string) {
            const char *s = json_object_get_string(val);
            if (!s || !s[0]) return 0;
            char *end = nullptr;
            errno = 0;
            long v = strtol(s, &end, 10);
            int parse_errno = errno;
            if (end == s || *end != '\0' || parse_errno == ERANGE) return 0;
            if (v < INT_MIN || v > INT_MAX) return 0;
            return static_cast<int>(v);
        }
    }
    return 0;
}

static int64_t get_json_int64(json_object *obj, const char *key) {
    json_object *val;
    if (json_object_object_get_ex(obj, key, &val)) {
        if (json_object_get_type(val) == json_type_int) {
            return json_object_get_int64(val);
        } else if (json_object_get_type(val) == json_type_string) {
            const char *s = json_object_get_string(val);
            if (!s || !s[0]) return 0;
            char *end = nullptr;
            errno = 0;
            long long v = strtoll(s, &end, 10);
            int parse_errno = errno;
            if (end == s || *end != '\0' || parse_errno == ERANGE) return 0;
            return v;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  JSON persistence                                                          */
/* -------------------------------------------------------------------------- */

/* Attaches `val` to `obj` under `key`, taking ownership either way: on failure `val` is
 * released here, so the caller only ever has to release `obj`. */
static bool json_add(json_object *obj, const char *key, json_object *val) {
    if (!val) return false;
    if (json_object_object_add(obj, key, val) != 0) {
        json_object_put(val);
        return false;
    }
    return true;
}

static bool write_mods_json(const std::string& filename, const std::vector<ModData>& mods) {
    json_object *jarray = json_object_new_array();
    if (!jarray) return false;

    for (const auto& mod : mods) {
        json_object *jobj = json_object_new_object();
        if (!jobj) { json_object_put(jarray); return false; }

        bool ok = json_add(jobj, "Id",             json_object_new_int(mod.Id))
               && json_add(jobj, "Name",           json_object_new_string(mod.Name.c_str()))
               && json_add(jobj, "Author",         json_object_new_string(mod.Author.c_str()))
               && json_add(jobj, "ThumbnailUrl",   json_object_new_string(mod.ThumbnailUrl.c_str()))
               && json_add(jobj, "LatestFileUrl",  json_object_new_string(mod.LatestFileUrl.c_str()))
               && json_add(jobj, "LatestFileDate", json_object_new_int64(mod.LatestFileDate))
               && json_add(jobj, "LatestFileName", json_object_new_string(mod.LatestFileName.c_str()));

        if (!ok || json_object_array_add(jarray, jobj) != 0) {
            json_object_put(jobj);   /* json_object_put cascades to children */
            json_object_put(jarray);
            return false;
        }
    }

    int res = json_object_to_file_ext(filename.c_str(), jarray, JSON_C_TO_STRING_NOSLASHESCAPE);
    json_object_put(jarray);
    return (res >= 0);
}

/* Loads one of the lists written by write_mods_json() back into memory. */
static bool read_mods_json(const char *filename, std::vector<ModData>& out) {
    out.clear();

    json_object *root = json_object_from_file(filename);
    if (!root) return false;
    if (json_object_get_type(root) != json_type_array) {
        json_object_put(root);
        return false;
    }

    int len = json_object_array_length(root);
    if (len > 0) out.reserve(static_cast<size_t>(len));

    for (int i = 0; i < len; i++) {
        json_object *rec = json_object_array_get_idx(root, i);
        if (!rec || json_object_get_type(rec) != json_type_object) continue;

        ModData mod;
        mod.Id = get_json_int(rec, "Id");
        if (mod.Id == 0) continue;

        const char *s;
        if ((s = get_json_string(rec, "Name"))           != nullptr) mod.Name           = s;
        if ((s = get_json_string(rec, "Author"))         != nullptr) mod.Author         = s;
        if ((s = get_json_string(rec, "ThumbnailUrl"))   != nullptr) mod.ThumbnailUrl   = s;
        if ((s = get_json_string(rec, "LatestFileUrl"))  != nullptr) mod.LatestFileUrl  = s;
        if ((s = get_json_string(rec, "LatestFileName")) != nullptr) mod.LatestFileName = s;
        mod.LatestFileDate = get_json_int64(rec, "LatestFileDate");

        out.push_back(std::move(mod));
    }

    json_object_put(root);
    return true;
}

/* installed_mods.json is an object keyed by the mod id as a decimal string, matching
 * the format StateService writes on the desktop side. */
static bool load_installed_mods() {
    g_installed.clear();

    json_object *root = json_object_from_file(INSTALLED_FILE);
    if (!root) return true;                       /* absent: nothing installed yet */
    if (json_object_get_type(root) != json_type_object) {
        json_object_put(root);
        return false;
    }

    json_object_object_foreach(root, key, val) {
        if (!key || !val || json_object_get_type(val) != json_type_object) continue;

        char *end = nullptr;
        errno = 0;
        long id = strtol(key, &end, 10);
        if (end == key || *end != '\0' || errno == ERANGE || id <= 0 || id > INT_MAX) continue;

        InstallRecord rec;
        rec.Date = get_json_int64(val, "Date");
        const char *src = get_json_string(val, "SourceFileName");
        if (src) rec.SourceFileName = src;

        json_object *files = nullptr;
        if (json_object_object_get_ex(val, "Files", &files) &&
            json_object_get_type(files) == json_type_array) {
            int n = json_object_array_length(files);
            for (int i = 0; i < n; i++) {
                json_object *entry = json_object_array_get_idx(files, i);
                if (!entry || json_object_get_type(entry) != json_type_string) continue;
                const char *fname = json_object_get_string(entry);
                if (fname && fname[0]) rec.Files.push_back(fname);
            }
        }

        g_installed[static_cast<int>(id)] = std::move(rec);
    }

    json_object_put(root);
    return true;
}

static bool save_installed_mods() {
    json_object *root = json_object_new_object();
    if (!root) return false;

    bool ok = true;
    for (auto it = g_installed.begin(); it != g_installed.end(); ++it) {
        json_object *jrec = json_object_new_object();
        if (!jrec) { ok = false; break; }

        /* Attach the array before filling it so `jrec` owns it and a single put()
         * unwinds the whole record on any later failure. */
        json_object *jfiles = json_object_new_array();
        if (!json_add(jrec, "Files", jfiles)) { json_object_put(jrec); ok = false; break; }

        for (const std::string& f : it->second.Files) {
            json_object *jf = json_object_new_string(f.c_str());
            if (!jf || json_object_array_add(jfiles, jf) != 0) {
                if (jf) json_object_put(jf);
                ok = false;
                break;
            }
        }

        if (ok) {
            ok = json_add(jrec, "Date", json_object_new_int64(it->second.Date))
              && json_add(jrec, "SourceFileName",
                          json_object_new_string(it->second.SourceFileName.c_str()));
        }
        if (!ok) { json_object_put(jrec); break; }

        /* json_add takes ownership of jrec on both paths from here. */
        if (!json_add(root, std::to_string(it->first).c_str(), jrec)) { ok = false; break; }
    }

    if (ok) {
        ok = (json_object_to_file_ext(INSTALLED_FILE, root, JSON_C_TO_STRING_NOSLASHESCAPE) >= 0);
    }
    json_object_put(root);
    return ok;
}

/* -------------------------------------------------------------------------- */
/*  GameBanana fetching                                                       */
/* -------------------------------------------------------------------------- */

/* Pulls the mod records out of one _aRecords array. */
static void parse_index_records(json_object *records, std::vector<ModData>& out) {
    const int len = json_object_array_length(records);
    for (int i = 0; i < len; i++) {
        json_object *record = json_object_array_get_idx(records, i);
        if (!record || json_object_get_type(record) != json_type_object) continue;

        const int id = get_json_int(record, "_idRow");
        if (id == 0) continue;

        ModData mod;
        mod.Id = id;

        const char *sName = get_json_string(record, "_sName");
        if (sName) mod.Name = sName;

        mod.Author = "Unknown";
        json_object *submitter;
        if (json_object_object_get_ex(record, "_aSubmitter", &submitter)
            && json_object_get_type(submitter) == json_type_object) {
            const char *subName = get_json_string(submitter, "_sName");
            if (subName) mod.Author = subName;
        }

        json_object *preview;
        if (json_object_object_get_ex(record, "_aPreviewMedia", &preview)
            && json_object_get_type(preview) == json_type_object) {
            json_object *images;
            if (json_object_object_get_ex(preview, "_aImages", &images)
                && json_object_get_type(images) == json_type_array) {
                int img_count = json_object_array_length(images);
                if (img_count > 0) {
                    json_object *img = json_object_array_get_idx(images, 0);
                    if (img && json_object_get_type(img) == json_type_object) {
                        const char *base_url = get_json_string(img, "_sBaseUrl");
                        const char *file220  = get_json_string(img, "_sFile220");
                        const char *file     = get_json_string(img, "_sFile");

                        if (base_url && file220 && base_url[0] && file220[0]) {
                            mod.ThumbnailUrl = std::string(base_url) + "/" + file220;
                        } else if (base_url && file && base_url[0] && file[0]) {
                            mod.ThumbnailUrl = std::string(base_url) + "/" + file;
                        }
                    }
                }
            }
        }

        out.push_back(std::move(mod));
    }
}

struct PageResult {
    std::vector<ModData> mods;
    int                  record_count = -1;   /* _aMetadata._nRecordCount, or -1 */
    bool                 ok           = false;
};

/* One page of one category. Appends to `out.mods` so a sequential run can accumulate. */
static bool fetch_index_page(CURL *curl, int cat_id, int page, PageResult& out) {
    char url[224];
    snprintf(url, sizeof(url), "%s?_nPage=%d&_nPerpage=%d&_aFilters[Generic_Category]=%d",
             API_V10_INDEX, page, INDEX_PER_PAGE, cat_id);

    const FetchResult result = curl_get_retry(curl, url, MAX_FETCH_ATTEMPTS);
    if (!result.success) return false;

    json_object *root = json_tokener_parse(result.data.c_str());
    if (!root) return false;

    json_object *meta;
    if (json_object_object_get_ex(root, "_aMetadata", &meta)
        && json_object_get_type(meta) == json_type_object) {
        out.record_count = get_json_int(meta, "_nRecordCount");
    }

    json_object *records;
    if (json_object_object_get_ex(root, "_aRecords", &records)
        && json_object_get_type(records) == json_type_array) {
        parse_index_records(records, out.mods);
    }

    json_object_put(root);
    out.ok = true;
    return true;
}

/* A page job is either one exact page, or a page plus every page after it until the
 * listing runs short. The "sequential" form is what guarantees nothing is missed when
 * the metadata is absent or undercounts. */
struct PageJob {
    int  cat_id     = 0;
    int  page       = 1;
    bool sequential = false;
};

struct IndexCtx {
    const PageJob *jobs    = nullptr;
    PageResult    *results = nullptr;
};

static void index_job(CURL *curl, size_t i, void *user) {
    IndexCtx *ctx = static_cast<IndexCtx*>(user);
    const PageJob& job = ctx->jobs[i];
    PageResult&    out = ctx->results[i];

    if (!job.sequential) {
        fetch_index_page(curl, job.cat_id, job.page, out);
    } else {
        for (int page = job.page; page < job.page + MAX_PAGES_PER_CAT; page++) {
            const size_t before = out.mods.size();
            if (!fetch_index_page(curl, job.cat_id, page, out)) break;
            if (out.mods.size() - before < static_cast<size_t>(INDEX_PER_PAGE)) break;
        }
    }
    report_progress();
}

static void collect_pages(const std::vector<PageJob>& jobs, std::vector<PageResult>& results) {
    results.clear();
    results.resize(jobs.size());
    if (jobs.empty()) return;

    IndexCtx ctx;
    ctx.jobs    = jobs.data();
    ctx.results = results.data();
    run_jobs(jobs.size(), index_job, &ctx);
}

static void deduplicate_mods(std::vector<ModData>& mods) {
    if (mods.size() <= 1) return;
    std::sort(mods.begin(), mods.end(), [](const ModData& a, const ModData& b) { return a.Id < b.Id; });
    auto last = std::unique(mods.begin(), mods.end(), [](const ModData& a, const ModData& b) { return a.Id == b.Id; });
    mods.erase(last, mods.end());
}

static void parse_latest_file(json_object *raw_item, ModData& mod) {
    json_object *item = raw_item;
    if (json_object_get_type(raw_item) == json_type_array) {
        int arr_len = json_object_array_length(raw_item);
        if (arr_len > 0) item = json_object_array_get_idx(raw_item, 0);
        else return;
    }

    if (!item || json_object_get_type(item) != json_type_object) return;

    int64_t max_ts = 0;
    json_object *files_obj = nullptr;

    auto process_file_obj = [&](json_object* file_obj) {
        if (file_obj && json_object_get_type(file_obj) == json_type_object) {
            int      fid = get_json_int(file_obj, "_idRow");
            int64_t  ts  = get_json_int64(file_obj, "_tsDateAdded");
            const char *sfile = get_json_string(file_obj, "_sFile");
            if (fid > 0 && sfile && sfile[0] && ts > max_ts) {
                max_ts = ts;
                mod.LatestFileUrl  = "https://gamebanana.com/dl/" + std::to_string(fid);
                mod.LatestFileName = sfile;
                mod.LatestFileDate = ts;
            }
        }
    };

    if (json_object_object_get_ex(item, "aFiles", &files_obj)
        && json_object_get_type(files_obj) == json_type_array) {
        int files_len = json_object_array_length(files_obj);
        if (files_len > 0) {
            for (int i = 0; i < files_len; i++) {
                process_file_obj(json_object_array_get_idx(files_obj, i));
            }
        }
    } else {
        json_object_object_foreach(item, key, file_obj) {
            (void)key;
            process_file_obj(file_obj);
        }
    }
}

/* Lowered only when the server unambiguously says a batch was too big, never on a
 * network hiccup. Once learned, later batches start at the known-good size instead of
 * rediscovering the limit one bisect at a time. */
static std::atomic<size_t> g_coreBatchCap(CORE_BATCH_SIZE);

static void lower_core_cap(size_t to) {
    if (to < 1) to = 1;
    size_t cur = g_coreBatchCap.load();
    while (to < cur && !g_coreBatchCap.compare_exchange_weak(cur, to)) { /* retry */ }
}

/* Resolves the latest file for mods[0..count). If the server rejects or mis-answers a
 * batch, the range is halved and retried down to single items, so an over-large batch
 * costs a little time but never loses records. */
static void fetch_core_range(CURL *curl, ModData *mods, size_t count) {
    if (count == 0) return;

    /* Honour a limit discovered earlier in this run. */
    if (count > 1 && count > g_coreBatchCap.load()) {
        const size_t half = count / 2;
        fetch_core_range(curl, mods, half);
        fetch_core_range(curl, mods + half, count - half);
        return;
    }

    std::vector<ModData*> targets;
    targets.reserve(count);

    std::string url = API_CORE_DATA;
    url += '?';
    for (size_t i = 0; i < count; i++) {
        if (!mods[i].LatestFileUrl.empty()) continue;   /* already resolved */
        url += "itemtype[]=Mod&itemid[]=";
        url += std::to_string(mods[i].Id);
        url += "&fields[]=Files().aFiles()&";
        targets.push_back(&mods[i]);
    }
    if (targets.empty()) return;
    url.erase(url.size() - 1);                          /* trailing '&' */

    bool ok       = false;
    bool too_big  = false;
    const FetchResult result = curl_get_retry(curl, url, MAX_FETCH_ATTEMPTS);

    if (result.success) {
        json_object *root = json_tokener_parse(result.data.c_str());
        if (root) {
            if (targets.size() == 1) {
                /* parse_latest_file unwraps either a bare object or a one-element array. */
                parse_latest_file(root, *targets[0]);
                ok = true;
            } else if (json_object_get_type(root) == json_type_array &&
                       static_cast<size_t>(json_object_array_length(root)) == targets.size()) {
                for (size_t k = 0; k < targets.size(); k++) {
                    json_object *item = json_object_array_get_idx(root, static_cast<int>(k));
                    if (item) parse_latest_file(item, *targets[k]);
                }
                ok = true;
            }
            json_object_put(root);
        }
        /* Answered, but not with one element per id: the batch was more than it would
         * take. A transport failure tells us nothing, so it must not count here. */
        too_big = !ok;
    } else if (is_request_too_large(result.response_code)) {
        too_big = true;
    }

    if (ok || count == 1) return;

    if (too_big) lower_core_cap(count / 2);

    const size_t half = count / 2;
    fetch_core_range(curl, mods, half);
    fetch_core_range(curl, mods + half, count - half);
}

struct CoreCtx {
    ModData *mods  = nullptr;
    size_t   total = 0;
};

static void core_job(CURL *curl, size_t i, void *user) {
    CoreCtx *ctx = static_cast<CoreCtx*>(user);

    /* Every job owns a disjoint slice, so workers never need to synchronise. */
    const size_t first = i * CORE_BATCH_SIZE;
    const size_t count = (first + CORE_BATCH_SIZE > ctx->total) ? ctx->total - first
                                                               : CORE_BATCH_SIZE;
    fetch_core_range(curl, ctx->mods + first, count);
    report_progress();
}

static void fetch_core_data(std::vector<ModData>& mods) {
    if (mods.empty()) return;

    CoreCtx ctx;
    ctx.mods  = mods.data();
    ctx.total = mods.size();

    const size_t batches = (mods.size() + CORE_BATCH_SIZE - 1) / CORE_BATCH_SIZE;
    begin_phase("Resolving downloads", batches);
    run_jobs(batches, core_job, &ctx);
}

/* -------------------------------------------------------------------------- */
/*  Background Fetch Thread                                                   */
/* -------------------------------------------------------------------------- */

/* Drains every page result into `out`, then releases the results. */
static void drain_pages(std::vector<PageResult>& results, std::vector<ModData>& out) {
    size_t incoming = 0;
    for (const PageResult& r : results) incoming += r.mods.size();
    out.reserve(out.size() + incoming);

    for (PageResult& r : results) {
        for (ModData& m : r.mods) out.push_back(std::move(m));
        std::vector<ModData>().swap(r.mods);
    }
}

static void fetch_thread_func(void* arg) {
    (void)arg;

    rmrf(CACHE_DIR);
    init_paths();
    share_init();

    {
        std::vector<ModData> all_mods;

        /* Pass 1: page 1 of every category, all at once. Besides the records, this is
         * what tells us how many pages each category actually has. */
        std::vector<PageJob> jobs(NUM_CATEGORIES);
        for (size_t i = 0; i < NUM_CATEGORIES; i++) {
            jobs[i].cat_id = CATEGORIES[i];
            jobs[i].page   = 1;
        }

        std::vector<PageResult> results;
        begin_phase("Scanning categories", jobs.size());
        collect_pages(jobs, results);

        /* Pass 2: every remaining page of every category, also all at once. The final
         * page of each category is fetched sequentially so that an undercounted
         * _nRecordCount still cannot cut a listing short. */
        std::vector<PageJob> more;
        for (size_t i = 0; i < NUM_CATEGORIES; i++) {
            const PageResult& r = results[i];
            if (!r.ok || r.mods.size() < static_cast<size_t>(INDEX_PER_PAGE)) continue;

            PageJob job;
            job.cat_id = CATEGORIES[i];

            if (r.record_count > INDEX_PER_PAGE) {
                int pages = (r.record_count + INDEX_PER_PAGE - 1) / INDEX_PER_PAGE;
                if (pages > MAX_PAGES_PER_CAT) pages = MAX_PAGES_PER_CAT;
                for (int p = 2; p < pages; p++) {
                    job.page = p;
                    job.sequential = false;
                    more.push_back(job);
                }
                job.page = pages;            /* keep going past here if it is still full */
            } else {
                job.page = 2;                /* no usable metadata: walk it */
            }
            job.sequential = true;
            more.push_back(job);
        }

        drain_pages(results, all_mods);

        if (!more.empty()) {
            std::vector<PageResult> more_results;
            begin_phase("Fetching mod pages", more.size());
            collect_pages(more, more_results);
            drain_pages(more_results, all_mods);
        }

        if (!all_mods.empty()) {
            deduplicate_mods(all_mods);
            fetch_core_data(all_mods);

            print_status("Saving...");

            size_t enriched = 0;
            for (const auto& m : all_mods) if (!m.LatestFileUrl.empty()) enriched++;

            if (!write_mods_json(MOD_LIST_FILE, all_mods)) {
                print_status("Error saving mod list!");
            }

            std::sort(all_mods.begin(), all_mods.end(), [](const ModData& a, const ModData& b) { return a.Name < b.Name; });
            if (!write_mods_json(BY_NAME_FILE, all_mods)) {
                print_status("Warning: failed to write byname.json");
            }

            std::sort(all_mods.begin(), all_mods.end(), [](const ModData& a, const ModData& b) { return a.LatestFileDate > b.LatestFileDate; });
            if (!write_mods_json(BY_UPDATED_FILE, all_mods)) {
                print_status("Warning: failed to write byupdated.json");
            }

            const int failures = g_failedRequests.load();
            if (failures > 0) {
                print_status("Done: %zu mods, %zu resolved, %d request(s) retried/failed.",
                             all_mods.size(), enriched, failures);
            } else {
                print_status("Done! Enriched %zu/%zu mods.", enriched, all_mods.size());
            }
        } else {
            print_status("Failed to fetch any mods!");
        }
    }

    share_cleanup();

    LightLock_Lock(&status_lock);
    g_fetchDone = true;
    LightLock_Unlock(&status_lock);
}

/* -------------------------------------------------------------------------- */
/*  Audio engine (libvorbisidec + NDSP)                                       */
/* -------------------------------------------------------------------------- */

static size_t ogg_read_cb(void *ptr, size_t size, size_t nmemb, void *datasource) {
    return fread(ptr, size, nmemb, static_cast<FILE*>(datasource));
}

static int ogg_seek_cb(void *datasource, ogg_int64_t offset, int whence) {
#if defined(__3DS__) && defined(fseeko)
    return fseeko(static_cast<FILE*>(datasource), static_cast<off_t>(offset), whence) ? -1 : 0;
#else
    if (offset < INT_MIN || offset > INT_MAX) return -1;
    return fseek(static_cast<FILE*>(datasource), static_cast<long>(offset), whence) ? -1 : 0;
#endif
}

static int ogg_close_cb(void *datasource) {
    return fclose(static_cast<FILE*>(datasource));
}

static long ogg_tell_cb(void *datasource) {
    return ftell(static_cast<FILE*>(datasource));
}

static ov_callbacks g_ovCbs{ogg_read_cb, ogg_seek_cb, ogg_close_cb, ogg_tell_cb};

static bool load_ogg(const char *path, OggVorbis_File *ovf) {
    ov_clear(ovf);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    int rc = ov_open_callbacks(f, ovf, nullptr, 0, g_ovCbs);
    if (rc < 0) { fclose(f); return false; }
    return true;
}

static void unload_ogg(OggVorbis_File *ovf) {
    if (ovf) ov_clear(ovf);
}

static void play_ogg(OggVorbis_File *ovf, bool loop) {
    if (!ovf) return;
    g_loopMode = loop;

    vorbis_info *info = ov_info(ovf, -1);
    if (!info) return;

    g_channels = info->channels < 1 ? 1 : info->channels > 2 ? 2 : info->channels;
    g_rate     = info->rate;

    ndspChnReset(0);
    ndspChnSetFormat(0, g_channels == 1 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetRate(0, static_cast<float>(g_rate));

    float mix[12] = {};
    mix[0] = mix[1] = AUDIO_VOL / 255.0f;
    ndspChnSetMix(0, mix);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetMasterVol(1.0f);
}

/* Decodes up to `frame_capacity` frames directly into dst (interleaved,
 * `channels` per frame). Returns the number of frames actually decoded and
 * sets *eos to true if the stream ended before the buffer was filled.
 * This decodes straight from libtremor for every call, so it naturally
 * interleaves decoding with NDSP playback instead of racing ahead to
 * decode a whole (arbitrarily long) file before any audio is queued. */
static int decode_frames(OggVorbis_File *ovf, s16 *dst, int frame_capacity, int channels, bool *eos) {
    *eos = false;
    const int bytes_capacity = frame_capacity * channels * (int)sizeof(s16);
    int bytes_done = 0;
    char *out = reinterpret_cast<char*>(dst);
    constexpr int MAX_CONSECUTIVE_ERRORS = 64;
    int consecutive_errors = 0;

    while (bytes_done < bytes_capacity) {
        int bitstream = 0;
        long br = ov_read(ovf, out + bytes_done, bytes_capacity - bytes_done, &bitstream);
        if (br == 0) { *eos = true; break; }               /* clean EOF */
        if (br < 0) {
            if (++consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                *eos = true; /* treat persistent stream error as EOS to prevent infinite spin */
                break;
            }
            continue;                                       /* recoverable stream error, retry */
        }
        consecutive_errors = 0;
        bytes_done += static_cast<int>(br);
    }
    return bytes_done / (channels * (int)sizeof(s16));
}

static void audio_thread_func(void *arg) {
    OggVorbis_File *intro = static_cast<OggVorbis_File*>(arg);
    if (!intro) return;

    play_ogg(intro, false);

    int cur = 0;
    bool first_buffer = true;

    while (!g_audioShouldStop) {
        ndspWaveBuf *wb = &g_wavebuf[cur];

        /* Don't touch this buffer's memory until NDSP is done playing it
         * (skip the wait on the very first fill; both buffers start idle). */
        if (!first_buffer) {
            while (wb->status != NDSP_WBUF_DONE && !g_audioShouldStop)
                svcSleepThread(1000000ULL);
            if (g_audioShouldStop) break;
        }
        first_buffer = false;

        bool eos = false;
        int frames = decode_frames(&g_ov, wb->data_pcm16, AUDIO_BUF_SAMPLES, g_channels, &eos);

        if (frames < AUDIO_BUF_SAMPLES) {
            /* Pad the tail with silence so we always hand NDSP a full buffer. */
            s16 *pad = wb->data_pcm16 + frames * g_channels;
            memset(pad, 0, (AUDIO_BUF_SAMPLES - frames) * g_channels * sizeof(s16));
        }

        wb->nsamples = AUDIO_BUF_SAMPLES;
        wb->status   = NDSP_WBUF_DONE;
        ndspChnWaveBufAdd(0, wb);
        cur ^= 1;

        if (eos) {
            if (!g_loopMode) {
                unload_ogg(&g_ov);
                g_ov = {};
                if (!load_ogg("romfs:/loop.ogg", &g_ov)) { print_status("Audio: loop.ogg missing"); break; }
                g_loopMode = true;
                play_ogg(&g_ov, true);
                first_buffer = true; /* ndspChnReset() inside play_ogg invalidated wavebuf state */
            } else {
                ov_raw_seek(&g_ov, 0);
            }
        }
    }
}

static void audio_init() {
    print_status("Audio: starting...");
    if (R_FAILED(ndspInit())) { print_status("Audio: ndspInit failed"); return; }
    g_ndspReady = true;
    print_status("Audio: ndspInit ok");

    if (!load_ogg("romfs:/intro.ogg", &g_ov)) { print_status("Audio: intro.ogg missing"); return; }
    print_status("Audio: intro.ogg loaded");

    int bs;
    char test_buf[4096];
    int test_read = ov_read(&g_ov, test_buf, sizeof(test_buf), &bs);
    if (test_read <= 0) {
        print_status("Audio: intro.ogg decode fail (%d)", test_read);
        unload_ogg(&g_ov);
        return;
    }
    ov_raw_seek(&g_ov, 0);

    vorbis_info *info = ov_info(&g_ov, -1);
    print_status("Audio: intro.ogg decodes ok (%d ch, %d Hz)",
                  info ? info->channels : 0, info ? info->rate : 0);

    const size_t wavebuf_bytes = AUDIO_BUF_SAMPLES * AUDIO_MAX_CHANNELS * sizeof(s16);
    g_wavebuf[0].data_pcm16 = static_cast<s16*>(linearAlloc(wavebuf_bytes));
    g_wavebuf[1].data_pcm16 = static_cast<s16*>(linearAlloc(wavebuf_bytes));
    // FIX: Proper cleanup on partial allocation failure
    if (!g_wavebuf[0].data_pcm16 || !g_wavebuf[1].data_pcm16) {
        print_status("Audio: linearAlloc fail");
        if (g_wavebuf[0].data_pcm16) linearFree(g_wavebuf[0].data_pcm16);
        if (g_wavebuf[1].data_pcm16) linearFree(g_wavebuf[1].data_pcm16);
        g_wavebuf[0].data_pcm16 = g_wavebuf[1].data_pcm16 = nullptr;
        return;
    }

    // FIX: Remove double-cast; implicit conversion is sufficient
    g_wavebuf[0].data_vaddr = g_wavebuf[0].data_pcm16;
    g_wavebuf[1].data_vaddr = g_wavebuf[1].data_pcm16;
    print_status("Audio: buffers allocated");

    memset(g_wavebuf[0].data_pcm16, 0, wavebuf_bytes);
    memset(g_wavebuf[1].data_pcm16, 0, wavebuf_bytes);
    g_wavebuf[0].nsamples = g_wavebuf[1].nsamples = AUDIO_BUF_SAMPLES;
    g_wavebuf[0].looping = g_wavebuf[1].looping = false;
    g_wavebuf[0].status  = g_wavebuf[1].status  = NDSP_WBUF_DONE;

    g_audioThread = threadCreate(audio_thread_func, &g_ov, 128 * 1024, 0x30, -2, true);
    if (!g_audioThread) { print_status("Audio: threadCreate fail"); return; }
    print_status("Audio: thread started");
}

/* Tears down in the reverse order of audio_init(), and is safe to call after a partial
 * or failed init: every step is guarded by the resource it releases. */
static void audio_shutdown() {
    g_audioShouldStop = true;
    if (g_audioThread) {
        threadJoin(g_audioThread, U64_MAX);
        threadFree(g_audioThread);
        g_audioThread = nullptr;
    }

    if (g_ndspReady) {
        ndspChnReset(0);
        ndspExit();
        g_ndspReady = false;
    }

    unload_ogg(&g_ov);

    /* Freed only after the DSP has stopped reading them. */
    for (int i = 0; i < 2; i++) {
        if (g_wavebuf[i].data_pcm16) {
            linearFree(g_wavebuf[i].data_pcm16);
            g_wavebuf[i].data_pcm16 = nullptr;
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Text measuring / fitting                                                  */
/* -------------------------------------------------------------------------- */

static const char *const ELLIPSIS = "\xE2\x80\xA6";   /* U+2026 */

static size_t utf8_char_len(unsigned char c) {
    if (c < 0x80)           return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;   /* stray continuation byte: consume one so we always advance */
}

static size_t utf8_step(const std::string& s, size_t i) {
    const size_t len = utf8_char_len(static_cast<unsigned char>(s[i]));
    return (len <= s.size() - i) ? len : 1;
}

/* Byte offset of the first byte of every character in `s`. */
static void utf8_char_starts(const std::string& s, std::vector<size_t>& out) {
    out.clear();
    for (size_t i = 0; i < s.size(); i += utf8_step(s, i)) out.push_back(i);
}

/* Shortens `in` until `measure` reports it fits `max_w` pixels. Binary search over
 * character positions, so a multi-byte codepoint is never split down the middle. */
template <typename MeasureFn>
static std::string fit_text(const std::string& in, float max_w, MeasureFn measure) {
    if (in.empty() || max_w <= 0.0f) return std::string();

    /* Bound the work (and the scratch text buffer) before measuring anything. */
    std::string src = in;
    if (src.size() > MEASURE_MAX_LEN) {
        size_t cut = 0;
        while (cut < src.size()) {
            const size_t step = utf8_step(src, cut);
            if (cut + step > MEASURE_MAX_LEN) break;
            cut += step;
        }
        src.resize(cut);
    }

    if (measure(src.c_str()) <= max_w) return src;

    std::vector<size_t> starts;
    utf8_char_starts(src, starts);

    int lo = 0, hi = static_cast<int>(starts.size()) - 1, best = 0;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const std::string cand = src.substr(0, starts[static_cast<size_t>(mid)]) + ELLIPSIS;
        if (measure(cand.c_str()) <= max_w) { best = mid; lo = mid + 1; }
        else                                { hi  = mid - 1; }
    }

    if (best <= 0) return std::string(ELLIPSIS);
    return src.substr(0, starts[static_cast<size_t>(best)]) + ELLIPSIS;
}

static float measure_c2d(const char *text, float scale) {
    if (!g_scratchBuf || !text || !text[0]) return 0.0f;
    C2D_TextBufClear(g_scratchBuf);
    C2D_Text t;
    if (!C2D_TextFontParseLine(&t, nullptr, g_scratchBuf, text, 0)) return 0.0f;
    return t.width * scale;
}

static float measure_imgui(const char *text) {
    return ImGui::CalcTextSize(text).x;
}

/* -------------------------------------------------------------------------- */
/*  UI lifetime                                                               */
/* -------------------------------------------------------------------------- */

static bool ui_init() {
    if (R_FAILED(fontEnsureMapped())) return false;

    /* The shared system font is authored at lineFeed pixels per line, so every scale
     * below is just "desired pixel height / authored height". */
    FINF_s *finf = C2D_FontGetInfo(nullptr);
    if (finf && finf->lineFeed > 0) g_fontLineH = static_cast<float>(finf->lineFeed);

    g_nameScale   = NAME_PX   / g_fontLineH;
    g_authorScale = AUTHOR_PX / g_fontLineH;
    g_statusScale = STATUS_PX / g_fontLineH;
    g_msgScale    = MSG_PX    / g_fontLineH;

    g_cardTextBuf = C2D_TextBufNew(CARD_TEXT_GLYPHS);
    g_scratchBuf  = C2D_TextBufNew(SCRATCH_GLYPHS);
    return g_cardTextBuf != nullptr && g_scratchBuf != nullptr;
}

static void ui_shutdown() {
    if (g_cardTextBuf) { C2D_TextBufDelete(g_cardTextBuf); g_cardTextBuf = nullptr; }
    if (g_scratchBuf)  { C2D_TextBufDelete(g_scratchBuf);  g_scratchBuf  = nullptr; }
    g_cardTextCount = 0;
    g_cardTextDirty = true;
}

/* -------------------------------------------------------------------------- */
/*  Browser model                                                             */
/* -------------------------------------------------------------------------- */

/* 3 = newer file upstream, 2 = installed and current, 1 = not installed.
 * Sorting descending on this is what floats updates to the top of every view. */
static int get_mod_priority(const ModData& mod) {
    std::map<int, InstallRecord>::const_iterator it = g_installed.find(mod.Id);
    if (it == g_installed.end()) return 1;
    return mod.LatestFileDate > it->second.Date ? 3 : 2;
}

/* Updates float above installed, installed above the rest; ties break on name or date
 * depending on the active sort. Matches GetSortedMods() upstream. */
static bool mod_sort_less(const ModData& a, const ModData& b) {
    const int pa = get_mod_priority(a), pb = get_mod_priority(b);
    if (pa != pb) return pa > pb;
    return g_sortByName ? a.Name < b.Name : a.LatestFileDate > b.LatestFileDate;
}

static void sort_mods() {
    std::stable_sort(g_mods.begin(), g_mods.end(), mod_sort_less);
    g_cardTextDirty = true;
}

static int mod_count() { return static_cast<int>(g_mods.size()); }

/* Cards currently on screen; the final page may be short. */
static int visible_count() {
    const int n = mod_count() - g_winStart;
    if (n <= 0) return 0;
    return n > CARDS_PER_PAGE ? CARDS_PER_PAGE : n;
}

/* Furthest row-aligned window start that still fills the screen from the bottom. */
static int max_win_start() {
    const int total = mod_count();
    if (total <= CARDS_PER_PAGE) return 0;
    const int rows      = (total + GRID_COLS - 1) / GRID_COLS;
    const int start_row = rows - GRID_ROWS;
    return start_row > 0 ? start_row * GRID_COLS : 0;
}

static const ModData *selected_mod() {
    const int n = visible_count();
    if (n <= 0 || g_selIdx < 0 || g_selIdx >= n) return nullptr;
    return &g_mods[static_cast<size_t>(g_winStart + g_selIdx)];
}

/* Re-sorts only the cards currently on screen and leaves the cursor exactly where it
 * is. ResortVisibleCards() upstream reorders the visible page in place and never
 * assigns SelectedIndex, so the highlight holds its grid position and whichever mod
 * lands there becomes the selection. */
static void resort_window() {
    const int n = visible_count();
    if (n <= 1) return;

    std::vector<ModData>::iterator first = g_mods.begin() + g_winStart;
    std::stable_sort(first, first + n, mod_sort_less);
    g_cardTextDirty = true;
}

static ModAction current_action() {
    const ModData *mod = selected_mod();
    if (!mod) return ACTION_NONE;
    std::map<int, InstallRecord>::const_iterator it = g_installed.find(mod->Id);
    if (it == g_installed.end()) return ACTION_INSTALL;
    return mod->LatestFileDate > it->second.Date ? ACTION_UPDATE : ACTION_INSTALLED;
}

/* Scrolls one row when the cursor enters the top or bottom row, so the window trails
 * the selection instead of snapping a page at a time (TryShiftAsync upstream). */
static void try_shift_window() {
    if (mod_count() <= CARDS_PER_PAGE) return;

    const int n         = visible_count();
    const int max_start = max_win_start();
    const int before    = g_winStart;

    if (g_selIdx >= n - GRID_COLS && g_winStart < max_start) {
        g_winStart += GRID_COLS;
        if (g_winStart > max_start) g_winStart = max_start;
        g_selIdx   -= (g_winStart - before);
    } else if (g_selIdx < GRID_COLS && g_winStart > 0) {
        g_winStart -= GRID_COLS;
        if (g_winStart < 0) g_winStart = 0;
        g_selIdx   += (before - g_winStart);
    }

    if (g_winStart == before) return;

    const int nv = visible_count();
    if (g_selIdx >= nv) g_selIdx = nv - 1;
    if (g_selIdx < 0)   g_selIdx = 0;
    g_cardTextDirty = true;
}

static void handle_nav(u32 keys) {
    const int n = visible_count();
    if (n <= 0) return;

    int dr = 0, dc = 0;
    if      (keys & (KEY_DRIGHT | KEY_CPAD_RIGHT)) dc =  1;
    else if (keys & (KEY_DLEFT  | KEY_CPAD_LEFT))  dc = -1;
    else if (keys & (KEY_DDOWN  | KEY_CPAD_DOWN))  dr =  1;
    else if (keys & (KEY_DUP    | KEY_CPAD_UP))    dr = -1;
    else return;

    const int max_row = (n - 1) / GRID_COLS;
    const int max_col = (n - 1) < (GRID_COLS - 1) ? (n - 1) : (GRID_COLS - 1);

    int row = g_selIdx / GRID_COLS + dr;
    int col = g_selIdx % GRID_COLS + dc;
    if (row < 0) row = 0; else if (row > max_row) row = max_row;
    if (col < 0) col = 0; else if (col > max_col) col = max_col;

    int idx = row * GRID_COLS + col;
    if (idx >= n) idx = n - 1;          /* the last row may be partial */

    const bool moved = (idx != g_selIdx);
    g_selIdx = idx;

    /* Vertical presses also shift when the cursor is already pinned to the edge row --
     * otherwise the index never changes there and the list could not be scrolled. */
    if (moved || dr != 0) try_shift_window();
}

/* Turns a held direction into a repeating stream of presses, standing in for the OS
 * key repeat the desktop build gets for free. */
static u32 nav_repeat(u32 kDown, u32 kHeld, float dt) {
    static constexpr u32 NAV_MASK =
        KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT |
        KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT;

    const u32 pressed = kDown & NAV_MASK;
    if (pressed) {                                   /* a new direction restarts the delay */
        g_navHeldKey = pressed;
        g_navTimer   = NAV_REPEAT_DELAY;
        return pressed;
    }
    if (!g_navHeldKey || !(kHeld & g_navHeldKey)) {
        g_navHeldKey = 0;
        return 0;
    }

    g_navTimer -= dt;
    if (g_navTimer > 0.0f) return 0;
    g_navTimer += NAV_REPEAT_RATE;
    if (g_navTimer < 0.0f) g_navTimer = 0.0f;        /* a long frame must not bank repeats */
    return g_navHeldKey;
}

/* -------------------------------------------------------------------------- */
/*  Install actions (placeholders)                                            */
/* -------------------------------------------------------------------------- */

/* Stands in for download + extract: records the mod as installed so the browse, sort
 * and uninstall paths can be exercised, and writes nothing to CTGP7_DIR.
 * NOTE: `mod` points into g_mods, which resort_window() reorders -- everything read
 * from it must be read before that call. */
static void fake_install(const ModData& mod) {
    const int id = mod.Id;

    InstallRecord rec;
    rec.Date           = mod.LatestFileDate;   /* matches InstallRecord(m.LatestFileDate, ...) */
    rec.SourceFileName = mod.LatestFileName.empty() ? std::string("fake.zip") : mod.LatestFileName;
    rec.Files.push_back(std::to_string(id) + ".chpack");

    g_installed[id] = std::move(rec);
    save_installed_mods();
    resort_window();
}

/* Stands in for deleting the extracted .chpack files and dropping the record. */
static void fake_uninstall(const ModData& mod) {
    if (g_installed.erase(mod.Id) == 0) return;
    save_installed_mods();
    resort_window();
}

/* Test affordance with no counterpart upstream: rolls the recorded install date back so
 * the mod flips to "Update Available", making the third card state reachable without
 * hand-editing installed_mods.json. Retire it along with fake_install(). */
static void fake_mark_outdated(const ModData& mod) {
    std::map<int, InstallRecord>::iterator it = g_installed.find(mod.Id);
    if (it == g_installed.end() || mod.LatestFileDate > it->second.Date) return;
    it->second.Date = mod.LatestFileDate - 1;
    save_installed_mods();
    resort_window();
}

static void do_action() {
    const ModData *mod = selected_mod();
    if (!mod) return;
    const ModAction action = current_action();
    if (action == ACTION_INSTALL || action == ACTION_UPDATE) fake_install(*mod);
}

static void do_uninstall() {
    const ModData *mod = selected_mod();
    if (mod && g_installed.count(mod->Id) != 0) fake_uninstall(*mod);
}

static void do_simulate_update() {
    const ModData *mod = selected_mod();
    if (mod) fake_mark_outdated(*mod);
}

/* Changing the sort re-sorts the whole list and jumps back to the top, the way
 * SetSortModeAsync() reloads the window at (0, 0) upstream. */
static void set_sort_mode(bool by_name) {
    if (g_sortByName == by_name) return;
    g_sortByName = by_name;
    sort_mods();
    g_winStart = 0;
    g_selIdx   = 0;
    g_cardTextDirty = true;
}

/* Loads the fetched list and hands control to the browser. */
static bool enter_browse_state() {
    load_installed_mods();

    const char *path = g_sortByName ? BY_NAME_FILE : BY_UPDATED_FILE;
    if (!read_mods_json(path, g_mods) || g_mods.empty()) {
        read_mods_json(MOD_LIST_FILE, g_mods);      /* fall back to the unsorted master */
    }
    if (g_mods.empty()) return false;

    g_winStart = 0;
    g_selIdx   = 0;
    sort_mods();
    return true;
}

/* -------------------------------------------------------------------------- */
/*  Top screen: the mod grid                                                  */
/* -------------------------------------------------------------------------- */

static void rebuild_card_text() {
    g_cardTextDirty = false;
    g_cardTextCount = 0;
    if (!g_cardTextBuf) return;

    C2D_TextBufClear(g_cardTextBuf);

    /* Parses one already-fitted label into the persistent buffer. */
    auto parse = [](const std::string& src, float scale, C2D_Text *out, float *out_w) -> bool {
        const std::string s = fit_text(src, TEXT_MAX_W,
            [scale](const char *c) { return measure_c2d(c, scale); });
        if (s.empty()) return false;
        if (!C2D_TextFontParseLine(out, nullptr, g_cardTextBuf, s.c_str(), 0)) return false;
        C2D_TextOptimize(out);
        *out_w = out->width * scale;
        return true;
    };

    const int n = visible_count();
    for (int i = 0; i < n; i++) {
        const ModData& mod = g_mods[static_cast<size_t>(g_winStart + i)];
        CardText& ct = g_cardText[i];

        ct.hasName   = parse(mod.Name,   g_nameScale,   &ct.name,   &ct.nameW);
        ct.hasAuthor = parse(mod.Author, g_authorScale, &ct.author, &ct.authorW);

        const int prio = get_mod_priority(mod);
        ct.hasStatus = (prio > 1) &&
            parse(prio == 3 ? "Update Available" : "Installed",
                  g_statusScale, &ct.status, &ct.statusW);
    }
    g_cardTextCount = n;
}

static void draw_mod_card(int slot, const ModData& mod, bool selected) {
    const float x = static_cast<float>(slot % GRID_COLS) * CELL_W + CARD_MARGIN;
    const float y = static_cast<float>(slot / GRID_COLS) * CELL_H + CARD_MARGIN;

    const int prio       = get_mod_priority(mod);
    const u32 status_clr = (prio == 3) ? CLR_AMBER : (prio == 2) ? CLR_GREEN : CLR_GOLD;

    /* Unselected cards paint border and fill in the window colour, exactly as
     * Theme.UnselBd / Theme.UnselBg do -- only the cursor draws a visible frame. */
    C2D_DrawRectSolid(x, y, 0.0f, CARD_W, CARD_H, selected ? status_clr : CLR_BG);

    const float cx = x + CARD_BORDER;
    const float cy = y + CARD_BORDER;
    C2D_DrawRectSolid(cx, cy, 0.0f, CONTENT_W, CONTENT_H, selected ? CLR_SEL_BG : CLR_BG);

    /* Thumbnail placeholder; real image loading drops in here later. */
    C2D_DrawRectSolid(cx, cy, 0.0f, CONTENT_W, THUMB_H, CLR_THUMB);

    if (slot >= g_cardTextCount) return;
    const CardText& ct = g_cardText[slot];

    if (ct.hasName) {
        C2D_DrawText(&ct.name, C2D_WithColor, cx + (CONTENT_W - ct.nameW) * 0.5f,
                     cy + NAME_Y, 0.0f, g_nameScale, g_nameScale, status_clr);
    }
    if (ct.hasAuthor) {
        C2D_DrawText(&ct.author, C2D_WithColor, cx + (CONTENT_W - ct.authorW) * 0.5f,
                     cy + AUTHOR_Y, 0.0f, g_authorScale, g_authorScale, CLR_AUTHOR);
    }
    if (ct.hasStatus) {
        C2D_DrawText(&ct.status, C2D_WithColor, cx + (CONTENT_W - ct.statusW) * 0.5f,
                     cy + STATUS_Y, 0.0f, g_statusScale, g_statusScale, status_clr);
    }
}

static void draw_top_message(const char *msg, u32 color) {
    if (!g_scratchBuf || !msg || !msg[0]) return;
    C2D_TextBufClear(g_scratchBuf);
    C2D_Text t;
    if (!C2D_TextFontParseLine(&t, nullptr, g_scratchBuf, msg, 0)) return;
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor,
                 (TOP_W - t.width * g_msgScale) * 0.5f,
                 (TOP_H - g_fontLineH * g_msgScale) * 0.5f,
                 0.0f, g_msgScale, g_msgScale, color);
}

static void draw_top_screen() {
    if (g_appState == STATE_FAILED) {
        draw_top_message(g_errorText.c_str(), CLR_ERROR);
        return;
    }
    if (g_appState != STATE_BROWSING) return;   /* blank while fetching, as upstream */

    if (g_cardTextDirty) rebuild_card_text();

    const int n = visible_count();
    if (n <= 0) {
        draw_top_message("No mods found.", CLR_ERROR);
        return;
    }
    for (int i = 0; i < n; i++) {
        draw_mod_card(i, g_mods[static_cast<size_t>(g_winStart + i)], i == g_selIdx);
    }
}

/* -------------------------------------------------------------------------- */
/*  Bottom screen: status and controls                                        */
/* -------------------------------------------------------------------------- */

static void imgui_text_centered(const char *text, const ImVec4& color, float y) {
    ImGui::SetCursorPos(ImVec2((BOT_W - ImGui::CalcTextSize(text).x) * 0.5f, y));
    ImGui::TextColored(color, "%s", text);
}

static void draw_bottom_status(const std::string& text, bool failed) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(BOT_W, BOT_H));
    ImGui::Begin("##status", nullptr, SCREEN_WINDOW_FLAGS);

    const ImVec4& color   = failed ? IM_ERROR : IM_GOLD;
    const float   wrap_w  = BOT_W - 20.0f;
    const ImVec2  one_line = ImGui::CalcTextSize(text.c_str());

    if (one_line.x <= wrap_w) {
        imgui_text_centered(text.c_str(), color, (BOT_H - one_line.y) * 0.5f);
    } else {
        /* Long progress lines wrap rather than run off the edge. */
        const ImVec2 wrapped = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap_w);
        ImGui::SetCursorPos(ImVec2(10.0f, (BOT_H - wrapped.y) * 0.5f));
        ImGui::PushTextWrapPos(10.0f + wrap_w);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }

    if (failed) imgui_text_centered("Press START to exit.", IM_AUTHOR, BOT_H - 40.0f);

    ImGui::End();
}

static std::string action_label(ModAction action, const ModData *mod) {
    if (action == ACTION_INSTALLED)    return "Installed";
    if (action == ACTION_NONE || !mod) return "No mods";
    /* No character cap: wrapped_button() lays this out over as many lines as fit and
     * only ellipsises what genuinely will not. */
    return std::string(action == ACTION_UPDATE ? "Update " : "Install ") + mod->Name;
}

/* Breaks `text` into at most `max_lines` lines that each fit `wrap_w`, ellipsising the
 * last one if there is still more to show. */
static void wrap_lines(const std::string& text, float wrap_w, int max_lines,
                       std::vector<std::string>& out) {
    out.clear();
    if (text.empty() || wrap_w <= 0.0f || max_lines <= 0) return;

    ImFont     *font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const char *s    = text.c_str();
    const char *end  = s + text.size();

    while (s < end && static_cast<int>(out.size()) < max_lines) {
        const char *stop = font->CalcWordWrapPosition(size, s, end, wrap_w);
        if (stop <= s) stop = s + 1;                    /* one word wider than the box */

        if (static_cast<int>(out.size()) == max_lines - 1 && stop < end) {
            out.push_back(fit_text(std::string(s, end), wrap_w, measure_imgui));
            return;
        }
        out.push_back(std::string(s, stop));

        s = stop;
        while (s < end && *s == ' ') s++;               /* eat the wrap point */
    }
}

/* A button whose label wraps and centres over multiple lines. ImGui::Button() is
 * single-line, which cut long mod names short even when the button had room to spare. */
static bool wrapped_button(const char *id, const std::string& label, float y, float height,
                           const ImVec4& bg, const ImVec4& bg_hot, const ImVec4& fg,
                           float border) {
    ImGui::SetCursorPos(ImVec2(BTN_X, y));
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(BTN_W, height));

    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const bool   hot = ImGui::IsItemHovered() || ImGui::IsItemActive();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(hot ? bg_hot : bg), BTN_ROUNDING);
    if (border > 0.0f) {
        dl->AddRect(p0, p1, ImGui::GetColorU32(fg), BTN_ROUNDING, 0, border);
    }

    std::vector<std::string> lines;
    wrap_lines(label, BTN_W - BTN_TEXT_PAD * 2.0f, BTN_MAX_LINES, lines);

    const float line_h = ImGui::GetTextLineHeight();
    const ImU32 col    = ImGui::GetColorU32(fg);
    float       ty     = p0.y + (height - line_h * static_cast<float>(lines.size())) * 0.5f;

    for (const std::string& line : lines) {
        const float tw = ImGui::CalcTextSize(line.c_str()).x;
        dl->AddText(ImVec2(p0.x + (BTN_W - tw) * 0.5f, ty), col, line.c_str());
        ty += line_h;
    }
    return pressed;
}

static void draw_bottom_browse() {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(BOT_W, BOT_H));
    ImGui::Begin("##browse", nullptr, SCREEN_WINDOW_FLAGS);

    const ImGuiStyle& style = ImGui::GetStyle();

    /* --- Sort options -------------------------------------------------------- */
    imgui_text_centered("Sort Options", IM_GOLD, SORT_LABEL_Y);
    {
        const float radio = ImGui::GetFrameHeight();
        const float w_name = radio + style.ItemInnerSpacing.x + ImGui::CalcTextSize("By Name").x;
        const float w_upd  = radio + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Recently Updated").x;

        ImGui::SetCursorPos(ImVec2((BOT_W - (w_name + style.ItemSpacing.x + w_upd)) * 0.5f, SORT_ROW_Y));
        if (ImGui::RadioButton("By Name", g_sortByName)) set_sort_mode(true);
        ImGui::SameLine(0.0f, style.ItemSpacing.x);
        if (ImGui::RadioButton("Recently Updated", !g_sortByName)) set_sort_mode(false);
    }

    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(BTN_X, SEP_Y), ImVec2(BTN_X + BTN_W, SEP_Y + 2.0f),
        IM_COL32(0x2A, 0x3B, 0x47, 0xFF));

    /* --- Action button ------------------------------------------------------- */
    {
        const ModAction   action = current_action();
        const std::string label  = action_label(action, selected_mod());
        const ImVec4&     fg     = (action == ACTION_UPDATE) ? IM_AMBER : IM_GOLD;

        ImGui::BeginDisabled(action != ACTION_INSTALL && action != ACTION_UPDATE);
        if (wrapped_button("##action", label, ACTION_BTN_Y, ACTION_BTN_H,
                           IM_BTN_BG, IM_BTN_HOT, fg, 2.0f)) {
            do_action();
        }
        ImGui::EndDisabled();
    }

    /* --- Uninstall button ---------------------------------------------------- */
    {
        /* Re-read: installing above may have re-sorted the page under us. The original
         * hides this button outright; here it stays put and greys out instead. */
        const ModData *mod = selected_mod();
        const bool can_uninstall = mod && g_installed.count(mod->Id) != 0;

        ImGui::BeginDisabled(!can_uninstall);
        if (wrapped_button("##uninstall", "Uninstall", UNINST_BTN_Y, UNINST_BTN_H,
                           IM_UNINST_BG, IM_UNINST_HOT, IM_UNINST_FG, 1.0f)) {
            do_uninstall();
        }
        ImGui::EndDisabled();
    }

    /* --- Position readout and button hints ----------------------------------- */
    {
        char buf[48];
        const int total = mod_count();
        const int cur   = (total > 0 && selected_mod()) ? g_winStart + g_selIdx + 1 : 0;
        snprintf(buf, sizeof(buf), "%d / %d", cur, total);
        imgui_text_centered(buf, IM_AUTHOR, COUNTER_Y);
    }
    imgui_text_centered("[A] Install   [B] Uninstall   [X] Sort", IM_AUTHOR, HINT1_Y);
    imgui_text_centered("[Y] Sim. update   [START] Exit", IM_AUTHOR, HINT2_Y);

    ImGui::End();
}

static void handle_browse_input(u32 navKeys, u32 kDown) {
    handle_nav(navKeys);

    if (kDown & KEY_A) do_action();
    if (kDown & KEY_B) do_uninstall();
    if (kDown & KEY_X) set_sort_mode(!g_sortByName);
    if (kDown & KEY_Y) do_simulate_update();
}

/* -------------------------------------------------------------------------- */
/*  Entry point                                                               */
/* -------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    LightLock_Init(&status_lock);

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    print_status("main: init complete...");

    bool fatal_error = false;
    bool imgui_ready = false;
    ImGuiIO* io = nullptr;
    TickCounter frameTime;
    touchPosition touch;
    u32 clrClear = CLR_BG;
    imgui_sw::SwOptions sw_options;
    Thread fetchThread = nullptr;
    bool romfs_mounted = false;

    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    if (!top || !bottom) {
        print_status("Fatal: failed to create render targets");
        fatal_error = true;
    } else {
        ImGui::CreateContext();
        imgui_ready = true;
        io = &ImGui::GetIO();
        io->DisplaySize = ImVec2(BOT_W, BOT_H);            // ImGui owns the bottom screen only
        imgui_sw::bind_imgui_painting(16.0f); // 3DS system font at 16px, same size the old UI used
        imgui_sw::make_style_fast();

        ImGuiStyle& style = ImGui::GetStyle();
        style.Colors[ImGuiCol_WindowBg] = MakeTextColor(0x15, 0x1D, 0x23);
        style.WindowRounding = 0.0f;
        style.WindowPadding = ImVec2(0.0f, 0.0f);
        style.WindowBorderSize = 0.0f;
        style.DisabledAlpha = 0.35f;                       // how far greyed-out controls fade

        io->DeltaTime = 1.0f / 60.0f;
        osTickCounterStart(&frameTime);

        if (!ui_init()) {
            print_status("Fatal: failed to set up text rendering");
            fatal_error = true;
        }
    }

    if (!fatal_error) {
        romfs_mounted = R_SUCCEEDED(romfsInit());
        if (!romfs_mounted) {
            print_status("romfsInit failed - no CA bundle!");
            fatal_error = true;
        } else {
            struct stat sb;
            if (stat("romfs:/cacert.pem", &sb) != 0) {
                print_status("Fatal: CA bundle missing from ROMFS");
                fatal_error = true;
            } else {
                audio_init();

                socBuffer = static_cast<u32*>(memalign(SOC_ALIGN, SOC_BUFFERSIZE));
                if (!socBuffer || R_FAILED(socInit(socBuffer, SOC_BUFFERSIZE))) {
                    print_status("socInit failed!");
                    fatal_error = true;
                } else {
                    if (R_FAILED(sslcInit(0))) {
                        print_status("sslcInit failed!");
                        fatal_error = true;
                    } else {
                        if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
                            print_status("curl_global_init failed!");
                            fatal_error = true;
                        } else {
                            fetchThread = threadCreate(fetch_thread_func, NULL, 128 * 1024, 0x3F, -2, true);
                            if (!fetchThread) {
                                print_status("Fatal: failed to create fetch thread");
                                fatal_error = true;
                            }
                        }
                    }
                }
            }
        }
    }

    while (!fatal_error && aptMainLoop()) {
        hidScanInput();
        const u32 kDown = hidKeysDown();
        const u32 kHeld = hidKeysHeld();
        const u32 kUp   = hidKeysUp();

        /* osTickCounterRead() reports milliseconds, not nanoseconds. */
        osTickCounterUpdate(&frameTime);
        float dt = static_cast<float>(osTickCounterRead(&frameTime) * 0.001);
        osTickCounterStart(&frameTime);
        if (!(dt > 0.0f)) dt = 1.0f / 60.0f;      /* ImGui requires a positive delta */
        io->DeltaTime = dt;

        hidTouchRead(&touch);
        if (kHeld & KEY_TOUCH) {
            io->MouseDown[0] = true;
            io->MousePos = ImVec2(static_cast<float>(touch.px), static_cast<float>(touch.py));
        } else {
            io->MouseDown[0] = false;
            /* On the frame the stylus lifts, hold the last position: ImGui only latches
             * a click if the widget is still hovered when the button goes up. */
            if (!(kUp & KEY_TOUCH)) io->MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        }

        LightLock_Lock(&status_lock);
        const bool done = g_fetchDone;
        LightLock_Unlock(&status_lock);

        /* Hand over to the browser once the fetch thread has published its lists. The
         * parse is deferred by one frame so "Loading mods..." is actually on screen
         * while it blocks: this frame paints the message, the next one does the work. */
        if (g_appState == STATE_FETCHING && done) {
            g_appState = STATE_LOADING;
            print_status("Loading mods...");
        } else if (g_appState == STATE_LOADING) {
            if (enter_browse_state()) {
                g_appState = STATE_BROWSING;
            } else {
                g_appState  = STATE_FAILED;
                g_errorText = "Failed to load mods.";
            }
        }

        /* Read after the transition above, so a status set this frame is the one drawn. */
        LightLock_Lock(&status_lock);
        const std::string currentText = g_statusText;
        LightLock_Unlock(&status_lock);

        if (done && (kDown & KEY_START)) break;

        if (g_appState == STATE_BROWSING) {
            handle_browse_input(nav_repeat(kDown, kHeld, dt), kDown);
        }

        /* Build the UI before opening the frame: nothing here touches the GPU, and it
         * lets the grid below render this frame's state rather than trailing it by one. */
        ImGui::NewFrame();
        if (g_appState == STATE_BROWSING) {
            draw_bottom_browse();
        } else {
            draw_bottom_status(g_appState == STATE_FAILED ? g_errorText : currentText,
                               g_appState == STATE_FAILED);
        }
        ImGui::Render();

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        // Top screen: the mod grid, drawn straight through citro2d
        C2D_TargetClear(top, clrClear);
        C2D_SceneBegin(top);
        C2D_Prepare();
        draw_top_screen();

        // Bottom screen: status text or the browser controls, via ImGui
        C2D_TargetClear(bottom, clrClear);
        C2D_SceneBegin(bottom);

        C2D_Prepare();
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        imgui_sw::paint_imgui(static_cast<int>(BOT_W), static_cast<int>(BOT_H), sw_options);
        C2D_Flush();

        C3D_FrameEnd(0);
    }

    /* Single teardown path: every step is guarded, so it is equally valid after a
     * fatal init failure and after a clean exit. Ordering matters -- the fetch thread
     * uses curl, and the audio thread reads its stream out of romfs. */
    if (fetchThread) {
        threadJoin(fetchThread, U64_MAX);
        threadFree(fetchThread);
    }

    curl_global_cleanup();
    sslcExit();
    if (socBuffer) {
        socExit();
        free(socBuffer);
        socBuffer = nullptr;
    }

    audio_shutdown();

    if (romfs_mounted) romfsExit();

    ui_shutdown();
    if (imgui_ready) {
        imgui_sw::unbind_imgui_painting();   /* releases its own C2D buffers first */
        ImGui::DestroyContext();
    }

    C2D_Fini();
    C3D_Fini();
    gfxExit();

    return fatal_error ? 1 : 0;
}