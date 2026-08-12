#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <curl/curl.h>
#include <json-c/json.h>

#include <stdio.h>      /* jpeglib.h needs FILE in scope */
#include <setjmp.h>
#include <jpeglib.h>

/* Safe after <3ds.h>: libctru's 3ds/archive.h exports only archiveMount*, and the
 * archive_read/archive_seek symbols in its devoptab object are file-local. */
#include <archive.h>
#include <archive_entry.h>

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
#define INSTALLED_TMP   APP_DIR "installed_mods.json.tmp"

/* Staging file for a mod download. Outside CTGP7_DIR so a partial archive is never
 * visible to CTGP-7, and a fixed name because only one install runs at a time -- each
 * install unlinks it up front, so a leftover from a crash self-heals. */
#define DOWNLOAD_TMP    CACHE_DIR "download.tmp"

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

/* The original's UniformGrid is 3 columns of ~124x123 cards (120x105 border, plus the
 * status line and margins). Its window is 400x240 -- the same size as this screen -- so
 * only about two rows are ever on screen; the rest of the 12 loaded cards sit below the
 * fold in a ScrollViewer. Matching that as a 3x2 visible grid gives cells of 133x120,
 * within a few pixels of the original card, and leaves the thumbnail room to render at
 * its full 110x62. Scrolling advances one row at a time, as the ScrollViewer does. */
static constexpr int GRID_COLS      = 3;
static constexpr int GRID_ROWS      = 2;
static constexpr int CARDS_PER_PAGE = GRID_COLS * GRID_ROWS;

static constexpr float CELL_W      = TOP_W / GRID_COLS;              /* 133.33 */
static constexpr float CELL_H      = TOP_H / GRID_ROWS;              /* 120    */
static constexpr float CARD_MARGIN = 2.0f;                           /* gap between cells    */
static constexpr float CARD_BORDER = 2.0f;                           /* BorderThickness="2"  */
static constexpr float CARD_W      = CELL_W - CARD_MARGIN * 2.0f;    /* 129.33 */
static constexpr float CARD_H      = CELL_H - CARD_MARGIN * 2.0f;    /* 116    */
static constexpr float CONTENT_W   = CARD_W - CARD_BORDER * 2.0f;    /* 125.33 */
static constexpr float CONTENT_H   = CARD_H - CARD_BORDER * 2.0f;    /* 112    */
static constexpr float TEXT_MAX_W  = CONTENT_W - 4.0f;               /* 121.33 */

/* Row offsets are measured from the card's content origin and stack to CONTENT_H. The
 * thumbnail spans the full card width at the cached image's 110:62 aspect, so essentially
 * none of it is cropped away. Text heights are line heights, which is why they sit above
 * the original's em-based FontSize 10 / 8 / 10. */
static constexpr float THUMB_H   = 70.0f;
static constexpr float NAME_Y    = 71.0f, NAME_PX   = 14.0f;  /* original FontSize 10 */
static constexpr float AUTHOR_Y  = 85.0f, AUTHOR_PX = 11.0f;  /* original FontSize  8 */
static constexpr float STATUS_Y  = 97.0f, STATUS_PX = 14.0f;  /* original FontSize 10 */
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
/* Install progress bar, in the free band between the action and uninstall buttons
 * (action ends at 106, uninstall starts at 114). */
static constexpr float PROG_BAR_Y   = 108.0f;
static constexpr float PROG_BAR_H   =   4.0f;

/* An install error replaces both hint lines rather than needing space of its own. */
static constexpr float MSG_LINE_Y   = 168.0f;
static constexpr int   MSG_MAX_LINES =   3;

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

/* libctru translates every path-taking call on sdmc:/romfs: (fopen, unlink, rename,
 * mkdir, stat, opendir) through two process-global scratch buffers -- __ctru_dev_path_buf
 * and __ctru_dev_utf16_buf -- and takes no lock of its own while doing it. Two threads
 * doing filesystem work at once can therefore corrupt each other's path. Reads and writes
 * on an already-open handle carry no path and are safe.
 *
 * So every path-taking call in this program goes through this lock. It is recursive
 * because the wrapped helpers call each other (init_paths -> mkdir_p,
 * install_extract -> thumb-style write helpers) and LightLock is not re-entrant.
 * Initialised at the very top of main(), before any thread exists. */
static RecursiveLock g_sdPathLock;

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

/* Scoped hold on g_sdPathLock. Every path-taking call in this file is wrapped by one of
 * these, at the leaf, so callers never have to remember. */
struct SdPathGuard {
    SdPathGuard()  { RecursiveLock_Lock(&g_sdPathLock);   }
    ~SdPathGuard() { RecursiveLock_Unlock(&g_sdPathLock); }
private:
    SdPathGuard(const SdPathGuard&);
    SdPathGuard& operator=(const SdPathGuard&);
};

static int rmrf(const char *path) {
    SdPathGuard sd;

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
    SdPathGuard sd;

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

    int res;
    {
        SdPathGuard sd;
        res = json_object_to_file_ext(filename.c_str(), jarray, JSON_C_TO_STRING_NOSLASHESCAPE);
    }
    json_object_put(jarray);
    return (res >= 0);
}

/* Loads one of the lists written by write_mods_json() back into memory. */
static bool read_mods_json(const char *filename, std::vector<ModData>& out) {
    out.clear();

    json_object *root;
    {
        SdPathGuard sd;
        root = json_object_from_file(filename);
    }
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

    json_object *root;
    {
        SdPathGuard sd;
        root = json_object_from_file(INSTALLED_FILE);
        /* Crashed between the unlink and the rename in save_installed_mods: the tmp is
         * then the only complete copy. Mirrors StateService.TryLoad's fallback. */
        if (!root) root = json_object_from_file(INSTALLED_TMP);
    }
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

    /* A stale tmp survived a crash but the real file loaded fine -- drop it, exactly as
     * StateService.TryLoad does, so the fallback above stays unambiguous. */
    {
        SdPathGuard sd;
        unlink(INSTALLED_TMP);
    }
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

    /* Write to a tmp and swap it in. This is the only record of what is on the card, and
     * load_installed_mods() cannot tell a truncated file from an absent one -- so a
     * half-written save would silently reset the whole install database. The window for
     * that is real on a 3DS: lid close, HOME, or APT termination mid-write. */
    if (ok) {
        SdPathGuard sd;
        ok = (json_object_to_file_ext(INSTALLED_TMP, root, JSON_C_TO_STRING_NOSLASHESCAPE) >= 0);
        if (ok) {
            unlink(INSTALLED_FILE);      /* 3DS rename() fails if the target exists */
            if (rename(INSTALLED_TMP, INSTALLED_FILE) != 0) {
                unlink(INSTALLED_TMP);
                ok = false;
            }
        } else {
            unlink(INSTALLED_TMP);
        }
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

    /* Only the listings are forced stale each launch. cache/images/ is keyed by mod id and
     * never goes stale, and re-downloading thousands of thumbnails every run would be
     * absurd -- same reasoning as keeping installed_mods.json outside CACHE_DIR. */
    rmrf(LISTS_DIR);
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

    /* The audio thread reopens loop.ogg mid-session, concurrently with SD work on other
     * threads -- and romfs: shares libctru's global path buffers with sdmc:. */
    FILE *f;
    {
        SdPathGuard sd;
        f = fopen(path, "rb");
    }
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

/* Installing or uninstalling changes a mod's priority, so the whole list is re-sorted --
 * not just the visible page. The mod then takes its place among *all* mods: updates
 * first, then installed, then the rest, each group ordered by the active sort. That is
 * what GetSortedMods() yields upstream once the window reloads.
 *
 * The cursor keeps its grid position rather than chasing the mod, matching
 * ResortVisibleCards(), which reorders cards and never assigns SelectedIndex. */
static void resort_after_install_change() {
    sort_mods();

    /* The list length cannot change here; these only guard a window already at the end. */
    const int max_start = max_win_start();
    if (g_winStart > max_start) g_winStart = max_start;
    if (g_winStart < 0)         g_winStart = 0;

    const int n = visible_count();
    if (g_selIdx >= n) g_selIdx = n - 1;
    if (g_selIdx < 0)  g_selIdx = 0;
    g_cardTextDirty = true;
}

static ModAction current_action() {
    const ModData *mod = selected_mod();
    if (!mod) return ACTION_NONE;
    std::map<int, InstallRecord>::const_iterator it = g_installed.find(mod->Id);
    if (it == g_installed.end()) return ACTION_INSTALL;
    return mod->LatestFileDate > it->second.Date ? ACTION_UPDATE : ACTION_INSTALLED;
}

/* Scrolls the window by one row, leaving the cursor on its slot so the incoming row
 * arrives under it. Returns false when there is nothing further that way. */
static bool scroll_window(int dr) {
    const int max_start = max_win_start();
    const int before    = g_winStart;

    g_winStart += dr * GRID_COLS;
    if (g_winStart < 0)         g_winStart = 0;
    if (g_winStart > max_start) g_winStart = max_start;
    if (g_winStart == before) return false;

    const int n = visible_count();
    if (g_selIdx >= n) g_selIdx = n - 1;
    if (g_selIdx < 0)  g_selIdx = 0;
    g_cardTextDirty = true;
    return true;
}

/* With only two rows on screen every cell sits in an edge row, so scrolling has to be
 * driven by direction rather than by "the cursor reached the edge" -- otherwise a
 * sideways move along the top row would scroll the list. */
static void handle_nav(u32 keys) {
    const int n = visible_count();
    if (n <= 0) return;

    int dr = 0, dc = 0;
    if      (keys & (KEY_DRIGHT | KEY_CPAD_RIGHT)) dc =  1;
    else if (keys & (KEY_DLEFT  | KEY_CPAD_LEFT))  dc = -1;
    else if (keys & (KEY_DDOWN  | KEY_CPAD_DOWN))  dr =  1;
    else if (keys & (KEY_DUP    | KEY_CPAD_UP))    dr = -1;
    else return;

    const int row = g_selIdx / GRID_COLS;
    const int col = g_selIdx % GRID_COLS;

    if (dc != 0) {
        /* Sideways stays within the row and never scrolls. */
        int nc = col + dc;
        if (nc < 0)             nc = 0;
        if (nc > GRID_COLS - 1) nc = GRID_COLS - 1;

        int idx = row * GRID_COLS + nc;
        if (idx >= n) idx = n - 1;      /* the last row may be partial */
        g_selIdx = idx;
        return;
    }

    const int max_row = (n - 1) / GRID_COLS;
    const int nr      = row + dr;
    if (nr >= 0 && nr <= max_row) {
        int idx = nr * GRID_COLS + col;
        if (idx >= n) idx = n - 1;
        g_selIdx = idx;
        return;
    }

    scroll_window(dr);   /* pinned to the edge row: bring the next row to the cursor */
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
/*  Install: download + extract                                               */
/* -------------------------------------------------------------------------- */

static constexpr const char *CHPACK_SUFFIX = ".chpack";   /* AppConfig.ModMemberSuffix */

static constexpr size_t  INSTALL_NAME_MAX        = 128;
static constexpr int     INSTALL_MAX_FILES       = 64;
static constexpr int     INSTALL_MAX_ENTRIES     = 20000;
static constexpr int64_t INSTALL_MAX_FILE_BYTES  = 64  * 1024 * 1024;
static constexpr int64_t INSTALL_MAX_TOTAL_BYTES = 256 * 1024 * 1024;
static constexpr size_t  ARCHIVE_BLOCK_SIZE      = 64 * 1024;

static constexpr curl_off_t DOWNLOAD_MAX_BYTES = 64 * 1024 * 1024;
static constexpr long    DL_CONNECT_TIMEOUT    = 20L;
static constexpr long    DL_LOW_SPEED_LIMIT    = 1024L;   /* bytes/sec ...      */
static constexpr long    DL_LOW_SPEED_TIME     = 30L;     /* ... for this long  */

static constexpr size_t  INSTALL_STACK_SIZE    = 256 * 1024;   /* libarchive + mbedTLS */
static constexpr s64     INSTALL_IDLE_WAIT_NS  = 50000000LL;

/* ASCII-only, deliberately not tolower(): tolower() on a negative char is undefined and
 * locale folding is wrong for filenames. Matches C#'s StringComparison.OrdinalIgnoreCase. */
static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool ascii_ends_with_ci(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    const size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return false;
    const char *tail = s + (ls - lf);
    for (size_t i = 0; i < lf; i++) {
        if (ascii_lower(tail[i]) != ascii_lower(suffix[i])) return false;
    }
    return true;
}

static bool ascii_iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

/* Reduces an archive entry name to a safe bare filename, or "" meaning skip it.
 * This is Path.GetFileName + ExtractFullPath=false, hardened -- entry names come from a
 * public mod site, so they are attacker-controlled. */
static std::string sanitize_entry_name(const char *raw) {
    if (!raw || !raw[0]) return std::string();

    /* Both separators. The ZIP spec says '/', but archives written by Windows tools do
     * contain '\\', and stripping only '/' would let "..\\..\\boot.firm" through whole. */
    const char *base = raw;
    for (const char *p = raw; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    const std::string name(base);
    if (name.empty()) return std::string();               /* trailing separator = a directory */
    if (name == "." || name == "..") return std::string();
    if (name.size() > INSTALL_NAME_MAX) return std::string();

    for (size_t i = 0; i < name.size(); i++) {
        const unsigned char c = (unsigned char)name[i];
        /* ':' matters even with no separator left: "C:evil" is drive-relative, and
         * "sdmc:" is itself a devoptab prefix. The rest is the FAT-illegal set. */
        if (c < 0x20 || c == 0x7F) return std::string();
        if (c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') return std::string();
    }
    return name;
}

/* UniversalExtractor gates on the extension of LatestFileName before doing any work, so
 * an unsupported mod costs nothing. */
static bool install_ext_supported(const std::string& fileName) {
    return ascii_ends_with_ci(fileName.c_str(), ".zip")
        || ascii_ends_with_ci(fileName.c_str(), ".7z")
        || ascii_ends_with_ci(fileName.c_str(), ".rar");
}

/* ---- worker-visible signals (defined before the helpers that poll them) ---- */
static std::atomic<bool>    g_instQuit(false);
static std::atomic<bool>    g_instCancel(false);
static std::atomic<int>     g_instPct(-1);
static std::atomic<int64_t> g_instBytes(0);
static std::atomic<int>     g_instFiles(0);   /* .chpack files written so far */

static bool install_aborting() { return g_instQuit.load() || g_instCancel.load(); }

struct ExtractOut {
    std::vector<std::string> files;
    char                     message[192] = {};
};

static void set_msg(char *dst, size_t cap, const char *text) {
    snprintf(dst, cap, "%s", text);
}

static void set_archive_msg(struct archive *a, char *dst, size_t cap, const char *what) {
    const char *detail = a ? archive_error_string(a) : nullptr;
    if (detail && detail[0]) snprintf(dst, cap, "%s: %s", what, detail);
    else                     snprintf(dst, cap, "%s.", what);
}

/* Extraction progress, measured in raw bytes consumed from the archive rather than in
 * entries. That is the only measure that works for every format: sizing or counting the
 * entries up front would mean a second pass, and for a solid archive a second pass means
 * decompressing the whole thing twice. It also advances while non-matching entries are
 * being skipped, so the bar never sits still. */
static void install_extract_progress(struct archive *a, int64_t archiveBytes, size_t files) {
    g_instFiles.store(static_cast<int>(files));
    if (archiveBytes <= 0) return;

    const la_int64_t done = archive_filter_bytes(a, -1);
    if (done < 0) return;

    int pct = static_cast<int>((static_cast<int64_t>(done) * 100) / archiveBytes);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    g_instPct.store(pct);
}

static bool write_zeros(FILE *f, int64_t count) {
    char zeros[512];
    memset(zeros, 0, sizeof(zeros));
    while (count > 0) {
        const size_t chunk = (count > (int64_t)sizeof(zeros)) ? sizeof(zeros) : (size_t)count;
        if (fwrite(zeros, 1, chunk, f) != chunk) return false;
        count -= (int64_t)chunk;
    }
    return true;
}

/* Extracts every *.chpack member into CTGP7_DIR. Returns the number written; 0 means
 * nothing matched or an error occurred (out->message says which).
 *
 * -fno-exceptions: one cleanup label, every local declared before the first goto. */
static int install_extract(const char *archivePath, ExtractOut *out) {
    struct archive       *a            = nullptr;
    struct archive_entry *entry        = nullptr;
    FILE                 *f            = nullptr;
    int64_t               total        = 0;
    int64_t               archiveBytes = 0;
    int                   entries      = 0;
    int                   r            = ARCHIVE_OK;
    bool                  already      = false;
    std::string           name;
    struct stat           st;
    char                  dst[320];
    char                  part[336];

    /* Denominator for the progress readout; 0 just means "unknown", not an error. */
    {
        SdPathGuard sd;
        if (stat(archivePath, &st) == 0 && st.st_size > 0) archiveBytes = (int64_t)st.st_size;
    }

    a = archive_read_new();
    if (!a) { set_msg(out->message, sizeof(out->message), "Out of memory."); goto cleanup; }

    /* Exactly the three container formats, and no stream filters: their compression lives
     * inside the container, so there is nothing for a filter to bid on. This also keeps
     * every other reader (tar/cpio/iso/...) and every filter out of the linked binary. */
    archive_read_support_format_zip(a);
    archive_read_support_format_7zip(a);
    archive_read_support_format_rar(a);    /* RAR 1.5 - 4.x */
    archive_read_support_format_rar5(a);   /* RAR 5.0+      */

    {
        /* open() and fstat() happen inside this call; every later read uses the fd, which
         * carries no path -- so this one guard covers the whole extract. */
        SdPathGuard sd;
        r = archive_read_open_filename(a, archivePath, ARCHIVE_BLOCK_SIZE);
    }
    if (r != ARCHIVE_OK) {
        set_archive_msg(a, out->message, sizeof(out->message), "Cannot open archive");
        goto cleanup;
    }

    for (;;) {
        if (install_aborting()) { set_msg(out->message, sizeof(out->message), "Cancelled."); goto cleanup; }
        if (++entries > INSTALL_MAX_ENTRIES) {
            set_msg(out->message, sizeof(out->message), "Archive has too many entries.");
            goto cleanup;
        }

        r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r == ARCHIVE_RETRY) continue;
        /* ARCHIVE_WARN is negative but the data is still valid, so the bail test is
         * "< ARCHIVE_WARN", never "< ARCHIVE_OK". */
        if (r < ARCHIVE_WARN) {
            set_archive_msg(a, out->message, sizeof(out->message), "Archive error");
            goto cleanup;
        }

        /* Keeps the bar moving across entries we skip, which in a big pack is most of them. */
        install_extract_progress(a, archiveBytes, out->files.size());

        if (archive_entry_filetype(entry) != AE_IFREG) continue;   /* dirs, links, devices */
        if (archive_entry_is_encrypted(entry)) {
            set_msg(out->message, sizeof(out->message), "Archive is password protected.");
            goto cleanup;
        }

        {
            /* The 3DS locale is "C", so the non-UTF-8 accessor can return NULL for a name
             * it cannot transcode. Ask for UTF-8 first. */
            const char *raw = archive_entry_pathname_utf8(entry);
            if (!raw) raw = archive_entry_pathname(entry);
            if (!raw) continue;
            if (!ascii_ends_with_ci(raw, CHPACK_SUFFIX)) continue;
            name = sanitize_entry_name(raw);
        }
        if (name.empty()) continue;

        /* Two entries can share a basename ("a/x.chpack" and "b/x.chpack"). Extract both
         * and let the later one win -- that is what ExtractionOptions.Overwrite = true
         * does upstream. Only the *record* is deduplicated: FAT is case-insensitive, so
         * they are one file on the card and listing it twice would make uninstall try the
         * same unlink twice. Skipping the entry outright (as this used to) silently
         * dropped a .chpack the archive genuinely contained. */
        already = false;
        for (size_t i = 0; i < out->files.size(); i++) {
            if (ascii_iequals(out->files[i], name)) { already = true; break; }
        }
        if (!already && (int)out->files.size() >= INSTALL_MAX_FILES) {
            set_msg(out->message, sizeof(out->message), "Archive has too many .chpack files.");
            goto cleanup;
        }

        snprintf(dst,  sizeof(dst),  "%s%s",      CTGP7_DIR, name.c_str());
        snprintf(part, sizeof(part), "%s%s.part", CTGP7_DIR, name.c_str());

        {
            SdPathGuard sd;
            f = fopen(part, "wb");
        }
        if (!f) { set_msg(out->message, sizeof(out->message), "Cannot write to the SD card."); goto cleanup; }

        {
            int64_t want = 0;   /* next logical offset we have written */
            for (;;) {
                const void *blk    = nullptr;
                size_t      blkLen = 0;
                la_int64_t  blkOff = 0;

                r = archive_read_data_block(a, &blk, &blkLen, &blkOff);
                if (r == ARCHIVE_EOF) { r = ARCHIVE_OK; break; }
                if (r < ARCHIVE_WARN) {
                    set_archive_msg(a, out->message, sizeof(out->message), "Extract failed");
                    goto cleanup;
                }
                if (blkLen == 0) continue;
                if (install_aborting()) { set_msg(out->message, sizeof(out->message), "Cancelled."); goto cleanup; }

                /* Sparse entries jump forward. FAT does not guarantee that a gap made by
                 * seeking past EOF reads back as zeros, so fill it explicitly. */
                if ((int64_t)blkOff < want) {
                    set_msg(out->message, sizeof(out->message), "Corrupt archive entry.");
                    goto cleanup;
                }
                if ((int64_t)blkOff > want) {
                    if (!write_zeros(f, (int64_t)blkOff - want)) {
                        set_msg(out->message, sizeof(out->message), "SD write failed.");
                        goto cleanup;
                    }
                    want = (int64_t)blkOff;
                }

                want  += (int64_t)blkLen;
                total += (int64_t)blkLen;
                if (want > INSTALL_MAX_FILE_BYTES || total > INSTALL_MAX_TOTAL_BYTES) {
                    set_msg(out->message, sizeof(out->message), "Archive is too large.");
                    goto cleanup;
                }
                if (fwrite(blk, 1, blkLen, f) != blkLen) {
                    set_msg(out->message, sizeof(out->message), "SD write failed (card full?).");
                    goto cleanup;
                }
                g_instBytes.store(total);
                install_extract_progress(a, archiveBytes, out->files.size());
            }
        }

        if (fclose(f) != 0) {
            f = nullptr;
            set_msg(out->message, sizeof(out->message), "SD write failed.");
            goto cleanup;
        }
        f = nullptr;

        {
            /* Swap the finished file in, so a truncated .chpack is never visible to
             * CTGP-7. rename() on the 3DS fails if the target exists. */
            SdPathGuard sd;
            unlink(dst);
            if (rename(part, dst) != 0) {
                unlink(part);
                set_msg(out->message, sizeof(out->message), "Cannot replace the installed file.");
                goto cleanup;
            }
        }
        if (!already) out->files.push_back(name);
        install_extract_progress(a, archiveBytes, out->files.size());
    }

    archive_read_free(a);
    return (int)out->files.size();

cleanup:
    if (f) {
        fclose(f);
        SdPathGuard sd;
        unlink(part);
    }
    if (a) archive_read_free(a);
    out->files.clear();
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  Install: streaming download                                               */
/* -------------------------------------------------------------------------- */

struct DownloadSink {
    FILE      *f       = nullptr;
    curl_off_t written = 0;
};

static size_t install_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    DownloadSink *sink = static_cast<DownloadSink*>(userp);
    const size_t total = size * nmemb;
    if (total == 0) return 0;

    if (sink->written > DOWNLOAD_MAX_BYTES - (curl_off_t)total) return CURL_WRITEFUNC_ERROR;
    if (fwrite(contents, 1, total, sink->f) != total)           return CURL_WRITEFUNC_ERROR;
    sink->written += (curl_off_t)total;
    return total;
}

/* libcurl calls this about once a second even on an idle connection, so shutdown and
 * cancel latency are both bounded at roughly that. */
static int install_xferinfo_cb(void*, curl_off_t dltotal, curl_off_t dlnow,
                               curl_off_t, curl_off_t) {
    if (install_aborting()) return 1;   /* -> CURLE_ABORTED_BY_CALLBACK */

    /* dltotal is 0 on the redirect leg and whenever Content-Length is absent. Publish -1
     * there rather than a percentage that would jump backwards when the CDN leg starts. */
    g_instPct.store(dltotal > 0 ? (int)((dlnow * 100) / dltotal) : -1);
    g_instBytes.store((int64_t)dlnow);
    return 0;
}

static void install_curl_configure(CURL *curl) {
    curl_configure(curl);

    /* curl_configure attaches g_curlShare, which the fetch thread destroys once the
     * listing finishes -- same reasoning as thumb_curl_configure. */
    curl_easy_setopt(curl, CURLOPT_SHARE, nullptr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, install_write_cb);

    /* Archives are already compressed, and transparent gunzip would make dltotal report
     * the encoded size, which would make the progress readout lie. */
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, nullptr);

    /* The inherited 45s TIMEOUT is a wall-clock cap on the whole transfer, which would
     * kill every large download over 3DS Wi-Fi. Replace it with a stall detector: a slow
     * but live transfer runs as long as it needs, a dead socket is dropped in 30s. */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, DL_CONNECT_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, DL_LOW_SPEED_LIMIT);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  DL_LOW_SPEED_TIME);

    /* Rejected before a byte of body arrives when Content-Length is present; the write
     * callback's own counter covers chunked responses. */
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)DOWNLOAD_MAX_BYTES);

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, install_xferinfo_cb);
}

/* LatestFileUrl is gamebanana.com/dl/<id>, which 302s to a CDN; FOLLOWLOCATION is already
 * set by curl_configure. Returns true only on a complete 2xx transfer. */
static bool install_download(CURL *curl, const std::string& url, char *msg, size_t msgLen) {
    DownloadSink sink;
    CURLcode     res  = CURLE_OK;
    long         code = 0;
    curl_off_t   reported = 0;

    {
        SdPathGuard sd;
        unlink(DOWNLOAD_TMP);
        sink.f = fopen(DOWNLOAD_TMP, "wb");
    }
    if (!sink.f) { set_msg(msg, msgLen, "Cannot write to the SD card."); return false; }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
    res = curl_easy_perform(curl);

    const bool closed = (fclose(sink.f) == 0);
    sink.f = nullptr;

    if (res != CURLE_OK) {
        if (res == CURLE_ABORTED_BY_CALLBACK)      set_msg(msg, msgLen, "Cancelled.");
        else if (res == CURLE_OPERATION_TIMEDOUT)  set_msg(msg, msgLen, "Download stalled.");
        else if (res == CURLE_WRITE_ERROR)         set_msg(msg, msgLen, "Download too large, or the card is full.");
        else snprintf(msg, msgLen, "Download failed: %s", curl_easy_strerror(res));
        return false;
    }
    if (!closed) { set_msg(msg, msgLen, "SD write failed."); return false; }

    /* Reports the LAST response, so a CDN 403 after a good 302 is caught here. */
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) != CURLE_OK ||
        code < 200 || code >= 300) {
        snprintf(msg, msgLen, "Server returned HTTP %ld.", code);
        return false;
    }
    /* Rather than trust that libcurl suppressed the 3xx bodies, check the arithmetic. */
    if (curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &reported) == CURLE_OK &&
        reported != sink.written) {
        set_msg(msg, msgLen, "Download was interrupted.");
        return false;
    }
    if (sink.written <= 0) { set_msg(msg, msgLen, "The server sent an empty file."); return false; }
    return true;
}

/* -------------------------------------------------------------------------- */
/*  Install: worker                                                           */
/* -------------------------------------------------------------------------- */

enum InstallPhase { INSTALL_IDLE = 0, INSTALL_DOWNLOADING, INSTALL_EXTRACTING, INSTALL_FINISHING };
enum InstallSlot  { SLOT_EMPTY = 0, SLOT_REQUESTED, SLOT_RUNNING, SLOT_COMPLETE };

/* Copied by value. Never a const ModData*: selected_mod() points into g_mods, and any
 * re-sort invalidates it -- keying by Id is what lets sorting stay live during an install. */
struct InstallReq {
    int         modId    = 0;
    int64_t     fileDate = 0;
    std::string url;
    std::string sourceName;
};

struct InstallRes {
    int                      modId    = 0;
    int64_t                  fileDate = 0;
    std::string              sourceName;
    std::vector<std::string> files;
    bool                     ok = false;
    char                     message[192] = {};
};

static LightLock       g_instLock;
static LightEvent      g_instWake;
static Thread          g_instThread   = nullptr;
static InstallSlot     g_instSlot     = SLOT_EMPTY;
static InstallReq      g_instReq;
static InstallRes      g_instRes;
static std::atomic<int> g_instPhase(INSTALL_IDLE);
static bool            g_instReady    = false;   /* main thread only */
static std::string     g_installMsg;             /* main thread only */

static void install_worker(void*) {
    CURL *curl = curl_easy_init();
    if (curl) install_curl_configure(curl);

    for (;;) {
        /* Armed before the predicate is tested; this is the only Clear in the design. */
        LightEvent_Clear(&g_instWake);

        InstallReq req;
        bool       have = false;

        LightLock_Lock(&g_instLock);
        if (!g_instQuit.load() && g_instSlot == SLOT_REQUESTED) {
            req        = g_instReq;
            g_instSlot = SLOT_RUNNING;
            have       = true;
        }
        LightLock_Unlock(&g_instLock);

        if (g_instQuit.load()) break;
        if (!have) { LightEvent_WaitTimeout(&g_instWake, INSTALL_IDLE_WAIT_NS); continue; }

        InstallRes res;
        res.modId      = req.modId;
        res.fileDate   = req.fileDate;
        res.sourceName = req.sourceName;

        g_instPhase.store(INSTALL_DOWNLOADING);
        g_instPct.store(-1);
        g_instBytes.store(0);

        if (!curl) {
            set_msg(res.message, sizeof(res.message), "Network is unavailable.");
        } else if (install_download(curl, req.url, res.message, sizeof(res.message))) {
            g_instPhase.store(INSTALL_EXTRACTING);
            g_instPct.store(-1);
            g_instBytes.store(0);
            g_instFiles.store(0);

            ExtractOut out;
            if (install_extract(DOWNLOAD_TMP, &out) > 0) {
                res.files = out.files;
                res.ok    = true;
            } else if (out.message[0]) {
                set_msg(res.message, sizeof(res.message), out.message);
            } else {
                set_msg(res.message, sizeof(res.message), "No .chpack files.");
            }
        }

        /* Mirrors ActionAsync's finally: runs on every path, including cancel. */
        {
            SdPathGuard sd;
            unlink(DOWNLOAD_TMP);
        }

        g_instPhase.store(INSTALL_FINISHING);

        LightLock_Lock(&g_instLock);
        g_instRes  = res;
        g_instSlot = SLOT_COMPLETE;
        LightLock_Unlock(&g_instLock);
        /* No ack wait: the main thread owns the slot from here, and waiting on an event
         * that a shutdown could have already consumed is exactly how a worker wedges. */
    }

    if (curl) curl_easy_cleanup(curl);
}

static bool install_busy() {
    if (!g_instReady) return false;
    LightLock_Lock(&g_instLock);
    const bool busy = (g_instSlot == SLOT_REQUESTED || g_instSlot == SLOT_RUNNING);
    LightLock_Unlock(&g_instLock);
    return busy;
}

static void install_cancel() {
    if (install_busy()) g_instCancel.store(true);
}

/* Queues an install/update. Everything the worker needs is copied here, on the main
 * thread, while `mod` is still known good. */
static bool install_begin(const ModData& mod) {
    if (!g_instReady || install_busy()) return false;

    if (mod.LatestFileUrl.empty()) { g_installMsg = "This mod has no download link."; return false; }
    if (!install_ext_supported(mod.LatestFileName)) {
        g_installMsg = "Unsupported archive type (only .zip, .7z and .rar).";
        return false;
    }

    InstallReq req;
    req.modId      = mod.Id;
    req.fileDate   = mod.LatestFileDate;
    req.url        = mod.LatestFileUrl;
    req.sourceName = mod.LatestFileName;

    g_instCancel.store(false);
    g_instPct.store(-1);
    g_instBytes.store(0);
    g_instFiles.store(0);
    g_instPhase.store(INSTALL_DOWNLOADING);
    g_installMsg.clear();

    LightLock_Lock(&g_instLock);
    const bool free_slot = (g_instSlot == SLOT_EMPTY);
    if (free_slot) {
        g_instReq  = req;
        g_instSlot = SLOT_REQUESTED;
    }
    LightLock_Unlock(&g_instLock);

    if (free_slot) LightEvent_Signal(&g_instWake);
    return free_slot;
}

/* Commits a finished install. Order matters: the previous version's leftovers are only
 * swept once the new one is fully on disk, so a failed update can never destroy a
 * working install. */
static void install_apply(const InstallRes& res) {
    std::map<int, InstallRecord>::iterator prev = g_installed.find(res.modId);
    if (prev != g_installed.end()) {
        for (size_t i = 0; i < prev->second.Files.size(); i++) {
            const std::string& old = prev->second.Files[i];

            /* Case-INSENSITIVE, because FAT is: "Mario.chpack" and "mario.chpack" are one
             * file on the card, and a case-sensitive diff here would delete the file the
             * update just wrote. */
            bool still_supplied = false;
            for (size_t k = 0; k < res.files.size(); k++) {
                if (ascii_iequals(old, res.files[k])) { still_supplied = true; break; }
            }
            if (still_supplied) continue;

            SdPathGuard sd;
            unlink((std::string(CTGP7_DIR) + old).c_str());   /* failures ignored, as upstream */
        }
    }

    InstallRecord rec;
    rec.Date           = res.fileDate;
    rec.Files          = res.files;
    rec.SourceFileName = res.sourceName;
    g_installed[res.modId] = rec;

    save_installed_mods();
    resort_after_install_change();
}

/* Runs on the main thread before input is handled, and deliberately NOT next to
 * thumbs_tick(): this mutates g_installed and re-sorts g_mods, which draw_bottom_browse()
 * reads before C3D_FrameBegin and draw_top_screen() reads after. Applying a result
 * between them would show one screen's old state against the other's new. */
static void install_tick() {
    if (!g_instReady) return;

    InstallRes res;
    bool have = false;

    LightLock_Lock(&g_instLock);
    if (g_instSlot == SLOT_COMPLETE) {
        res        = g_instRes;
        g_instRes  = InstallRes();
        g_instSlot = SLOT_EMPTY;
        have       = true;
    }
    LightLock_Unlock(&g_instLock);
    if (!have) return;

    g_instPhase.store(INSTALL_IDLE);
    g_instCancel.store(false);

    if (res.ok) {
        install_apply(res);
        g_installMsg.clear();
    } else {
        /* Leave g_installed untouched, exactly as ActionAsync's catch does: the old
         * record still describes the old install, so uninstall and retry both work. */
        g_installMsg = res.message[0] ? res.message : "Install failed.";
    }
}

static std::string install_progress_label() {
    const int phase = g_instPhase.load();

    if (phase == INSTALL_EXTRACTING) {
        const int pct   = g_instPct.load();
        const int files = g_instFiles.load();
        char buf[64];
        if (pct < 0) {
            snprintf(buf, sizeof(buf), "Extracting...");
        } else if (files > 0) {
            snprintf(buf, sizeof(buf), "Extracting %d%%  (%d file%s)",
                     pct, files, files == 1 ? "" : "s");
        } else {
            snprintf(buf, sizeof(buf), "Extracting %d%%", pct);
        }
        return std::string(buf);
    }
    if (phase == INSTALL_FINISHING)  return "Finishing...";

    const int pct = g_instPct.load();
    if (pct >= 0) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Downloading %d%%", pct < 100 ? pct : 100);
        return std::string(buf);
    }

    const int64_t kb = g_instBytes.load() / 1024;
    char buf[64];
    if (kb >= 1024) snprintf(buf, sizeof(buf), "Downloading %.1f MB", (double)kb / 1024.0);
    else            snprintf(buf, sizeof(buf), "Downloading %" PRId64 " KB", kb);
    return std::string(buf);
}

static bool install_init() {
    if (g_instReady) return true;

    LightLock_Init(&g_instLock);
    LightEvent_Init(&g_instWake, RESET_STICKY);
    g_instQuit.store(false);
    g_instCancel.store(false);
    g_instSlot = SLOT_EMPTY;

    /* Leftover from a crash mid-install. */
    {
        SdPathGuard sd;
        unlink(DOWNLOAD_TMP);
    }

    g_instReady  = true;
    g_instThread = threadCreate(install_worker, nullptr, INSTALL_STACK_SIZE,
                                WORKER_PRIORITY, -2, false);
    if (!g_instThread) { g_instReady = false; return false; }
    return true;
}

static void install_shutdown() {
    if (!g_instReady) return;
    g_instReady = false;

    g_instQuit.store(true);
    LightEvent_Signal(&g_instWake);

    if (g_instThread) {
        threadJoin(g_instThread, U64_MAX);
        threadFree(g_instThread);
        g_instThread = nullptr;
    }
    SdPathGuard sd;
    unlink(DOWNLOAD_TMP);
}

static void do_action() {
    if (install_busy()) return;
    const ModData *mod = selected_mod();
    if (!mod) return;
    const ModAction action = current_action();
    if (action == ACTION_INSTALL || action == ACTION_UPDATE) install_begin(*mod);
}

/* A handful of unlinks, so it stays on the main thread. Mirrors
 * StateService.UninstallAsync: drop the record, save, then delete the files. */
static void do_uninstall() {
    if (install_busy()) return;

    const ModData *mod = selected_mod();
    if (!mod) return;

    std::map<int, InstallRecord>::iterator it = g_installed.find(mod->Id);
    if (it == g_installed.end()) return;

    const std::vector<std::string> files = it->second.Files;
    g_installed.erase(it);
    save_installed_mods();

    for (size_t i = 0; i < files.size(); i++) {
        SdPathGuard sd;
        unlink((std::string(CTGP7_DIR) + files[i]).c_str());   /* failures ignored */
    }

    g_installMsg.clear();
    resort_after_install_change();
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

/* -------------------------------------------------------------------------- */
/*  Thumbnails                                                                */
/* -------------------------------------------------------------------------- */

/* The cache format is byte-compatible with the desktop original: a 110x62 JPEG at
 * quality 50 in cache/images/<modId>.jpg, cropped the same way. The directory can be
 * copied between the two builds in either direction. */
static constexpr int    THUMB_IMG_W        = 110;   /* AppConfig.FinalThumbnailWidth  */
static constexpr int    THUMB_IMG_H        = 62;    /* AppConfig.FinalThumbnailHeight */
static constexpr int    THUMB_JPEG_QUALITY = 50;    /* JpegEncoder { Quality = 50 }   */

/* Next power of two up from 110x62; the PICA200 only samples POT textures. */
static constexpr u16    THUMB_TEX_W     = 128;
static constexpr u16    THUMB_TEX_H     = 64;
static constexpr size_t THUMB_TEX_BYTES = (size_t)THUMB_TEX_W * THUMB_TEX_H * 2u;  /* RGB565 */

static constexpr int    THUMB_SLOTS     = 16;   /* 12 on screen + headroom to evict into */
static constexpr int    THUMB_WORKERS   = 3;
static constexpr int    THUMB_QUEUE     = CARDS_PER_PAGE;
static constexpr int    THUMB_FAIL_RING = 64;

static constexpr size_t THUMB_URL_MAX      = 192;
static constexpr size_t THUMB_PATH_MAX     = 128;
static constexpr size_t THUMB_MAX_BYTES    = 1024 * 1024;   /* _sFile fallback can be big */
static constexpr unsigned THUMB_SRC_MAX_DIM    = 4096;      /* decompression-bomb guards  */
static constexpr size_t   THUMB_SRC_MAX_PIXELS = 2000000;
static constexpr s64    THUMB_IDLE_WAIT_NS = 50000000LL;    /* 50ms                       */

struct RawImage { unsigned char *rgb; int w, h; };   /* POD: safe around setjmp */

struct ThumbSlot {
    C3D_Tex tex;
    int     modId;      /* 0 == empty */
    u32     lastUsed;
};

struct ThumbReq { int id; char url[THUMB_URL_MAX]; };

struct ThumbPub {
    u16       *pixels;      /* linearAlloc'd staging, swapped with a slot on publish */
    LightEvent ack;
    int        modId;
    bool       ready;
    bool       ok;
};

/* Slots are touched only by the main thread. Workers touch the queue, the in-flight
 * table and their own publish record -- never a C3D_Tex, g_mods or g_winStart. */
static ThumbSlot         g_thumbSlot[THUMB_SLOTS];
static ThumbPub          g_thumbPub[THUMB_WORKERS];
static Thread            g_thumbThread[THUMB_WORKERS];
static ThumbReq          g_thumbQueue[THUMB_QUEUE];
static int               g_thumbQueueN = 0;
static int               g_thumbFlight[THUMB_WORKERS];
static int               g_thumbFail[THUMB_FAIL_RING];
static int               g_thumbFailPos = 0;
static int               g_thumbWant[CARDS_PER_PAGE];
static int               g_thumbWantN   = 0;
static u32               g_thumbFrame   = 0;
static bool              g_thumbReady   = false;
static LightLock         g_thumbLock;
static LightEvent        g_thumbWake;
static std::atomic<bool> g_thumbQuit(false);
static Tex3DS_SubTexture g_thumbSubTex;

/* -------------------------------------------------------------------------- */
/*  JPEG decode / encode                                                      */
/* -------------------------------------------------------------------------- */

/* libjpeg reports errors by longjmp. The build is -fno-exceptions, so GCC emits no
 * cleanup paths: nothing with a non-trivial destructor may be live in either of the two
 * functions below, and every scalar live across the setjmp must be volatile or -Wclobbered
 * fires (and the value really would be undefined). cinfo and jerr are address-taken, which
 * forces them to memory, so they need neither. */
struct ThumbJpegErr {
    struct jpeg_error_mgr pub;
    jmp_buf               jb;
};

static void thumb_jpeg_error_exit(j_common_ptr cinfo) {
    longjmp(reinterpret_cast<ThumbJpegErr*>(cinfo->err)->jb, 1);
}

/* libjpeg's default writes to stderr, which is nowhere on a 3DS. */
static void thumb_jpeg_silence(j_common_ptr) { }

/* Smallest N in [1,8] where ceil(w*N/8) >= 110 and ceil(h*N/8) >= 62, i.e. the cheapest
 * DCT scaling that still leaves the crop enough pixels. (v*N+7)/8 is exactly libjpeg's
 * jdiv_round_up, so this agrees with jpeg_calc_output_dimensions. Requiring both axes is
 * a superset of what the crop needs, so it can never under-sample. The usual 220x124
 * source lands on N=4 and decodes straight to 110x62 for free. */
static int thumb_pick_scale_num(unsigned int w, unsigned int h) {
    for (unsigned int n = 1; n < 8; n++) {
        if ((w * n + 7u) / 8u >= (unsigned int)THUMB_IMG_W &&
            (h * n + 7u) / 8u >= (unsigned int)THUMB_IMG_H) {
            return (int)n;
        }
    }
    return 8;
}

static bool thumb_jpeg_decode(const unsigned char *src, size_t len, RawImage *out) {
    struct jpeg_decompress_struct cinfo;
    ThumbJpegErr                  jerr;
    unsigned char *volatile       rgb  = nullptr;
    volatile bool                 ok   = false;
    volatile int                  outW = 0;
    volatile int                  outH = 0;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit     = thumb_jpeg_error_exit;
    jerr.pub.output_message = thumb_jpeg_silence;

    /* Before create: jpeg_CreateDecompress nulls cinfo.mem first thing, so the
     * unconditional destroy below is valid even if creation itself longjmp'd. */
    if (setjmp(jerr.jb) == 0) {
        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, src, (unsigned long)len);

        if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK &&
            cinfo.image_width  > 0 && cinfo.image_width  <= THUMB_SRC_MAX_DIM &&
            cinfo.image_height > 0 && cinfo.image_height <= THUMB_SRC_MAX_DIM) {

            cinfo.scale_num   = (unsigned int)thumb_pick_scale_num(cinfo.image_width,
                                                                   cinfo.image_height);
            cinfo.scale_denom = 8;
            cinfo.out_color_space     = JCS_RGB;
            cinfo.dct_method          = JDCT_IFAST;
            cinfo.do_fancy_upsampling = FALSE;
            jpeg_calc_output_dimensions(&cinfo);

            /* Checked post-scaling and pre-allocation, so a bomb costs nothing. */
            if (cinfo.output_components == 3 &&
                (size_t)cinfo.output_width * (size_t)cinfo.output_height <= THUMB_SRC_MAX_PIXELS) {
                rgb = (unsigned char*)malloc((size_t)cinfo.output_width *
                                             (size_t)cinfo.output_height * 3u);
                if (rgb) {
                    jpeg_start_decompress(&cinfo);
                    while (cinfo.output_scanline < cinfo.output_height) {
                        JSAMPROW row = rgb + (size_t)cinfo.output_scanline *
                                             (size_t)cinfo.output_width * 3u;
                        if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) break;
                    }
                    ok   = (cinfo.output_scanline >= cinfo.output_height);
                    outW = (int)cinfo.output_width;
                    outH = (int)cinfo.output_height;
                    jpeg_finish_decompress(&cinfo);
                }
            }
        }
    }
    jpeg_destroy_decompress(&cinfo);

    if (!ok) { free(rgb); return false; }   /* free(nullptr) is well defined */
    out->rgb = rgb;
    out->w   = outW;
    out->h   = outH;
    return true;
}

static bool thumb_jpeg_encode(const unsigned char *rgb, unsigned char **outBuf,
                              unsigned long *outLen) {
    struct jpeg_compress_struct cinfo;
    ThumbJpegErr                jerr;
    unsigned char              *buf = nullptr;   /* address-taken -> forced to memory */
    unsigned long               len = 0;         /* likewise, so neither needs volatile */
    volatile bool               ok  = false;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit     = thumb_jpeg_error_exit;
    jerr.pub.output_message = thumb_jpeg_silence;

    if (setjmp(jerr.jb) == 0) {
        jpeg_create_compress(&cinfo);
        jpeg_mem_dest(&cinfo, &buf, &len);

        cinfo.image_width      = (JDIMENSION)THUMB_IMG_W;
        cinfo.image_height     = (JDIMENSION)THUMB_IMG_H;
        cinfo.input_components = 3;
        cinfo.in_color_space   = JCS_RGB;
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, THUMB_JPEG_QUALITY, TRUE);

        jpeg_start_compress(&cinfo, TRUE);
        while (cinfo.next_scanline < cinfo.image_height) {
            JSAMPROW row = const_cast<JSAMPROW>(rgb + (size_t)cinfo.next_scanline *
                                                      (size_t)THUMB_IMG_W * 3u);
            if (jpeg_write_scanlines(&cinfo, &row, 1) != 1) break;
        }
        /* Only claim success once finish_compress has returned -- it flushes the tail of
         * the stream, and a longjmp out of it would leave a truncated file. */
        if (cinfo.next_scanline >= cinfo.image_height) {
            jpeg_finish_compress(&cinfo);
            ok = true;
        }
    }
    jpeg_destroy_compress(&cinfo);

    if (!ok || !buf) { free(buf); return false; }
    *outBuf = buf;
    *outLen = len;
    return true;
}

/* -------------------------------------------------------------------------- */
/*  Crop, scale, tile                                                         */
/* -------------------------------------------------------------------------- */

/* Crops to the 110:62 aspect -- horizontally centred, top edge anchored (cy = 0, exactly
 * as ProcessImageAsync does) -- then box-filters down to 110x62. Lanczos3 like the desktop
 * build buys nothing visible at 92x38 through a quality-50 JPEG, and the ARM11 has no
 * vector unit; after DCT scaling this is only 1-4 taps per output pixel anyway. */
static void thumb_crop_resize(const RawImage *src, unsigned char *dst) {
    const int sw = src->w;
    const int sh = src->h;

    int cw, ch;
    if (sw * THUMB_IMG_H >= sh * THUMB_IMG_W) {     /* sw/sh >= 110/62, exactly */
        cw = (sh * THUMB_IMG_W) / THUMB_IMG_H;
        ch = sh;
    } else {
        cw = sw;
        ch = (sw * THUMB_IMG_H) / THUMB_IMG_W;
    }
    /* Integer truncation zeroes a dimension at extreme aspects (a 1px-wide source gives
     * ch = 0). ImageSharp throws there and the original silently drops the thumbnail;
     * clamping keeps the maths total and still yields something. */
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    int cx = (sw - cw) / 2;
    if (cx < 0) cx = 0;
    const int cy = 0;
    if (cw > sw - cx) cw = sw - cx;
    if (ch > sh)      ch = sh;

    for (int dy = 0; dy < THUMB_IMG_H; dy++) {
        int sy0 = cy + (dy * ch) / THUMB_IMG_H;
        int sy1 = cy + ((dy + 1) * ch) / THUMB_IMG_H;
        if (sy1 <= sy0)     sy1 = sy0 + 1;
        if (sy1 > cy + ch)  sy1 = cy + ch;

        for (int dx = 0; dx < THUMB_IMG_W; dx++) {
            int sx0 = cx + (dx * cw) / THUMB_IMG_W;
            int sx1 = cx + ((dx + 1) * cw) / THUMB_IMG_W;
            if (sx1 <= sx0)    sx1 = sx0 + 1;
            if (sx1 > cx + cw) sx1 = cx + cw;

            unsigned int r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const unsigned char *p = src->rgb + ((size_t)sy * (size_t)sw + (size_t)sx0) * 3u;
                for (int sx = sx0; sx < sx1; sx++, p += 3) {
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    n++;
                }
            }

            unsigned char *o = dst + ((size_t)dy * (size_t)THUMB_IMG_W + (size_t)dx) * 3u;
            o[0] = (unsigned char)(r / n);
            o[1] = (unsigned char)(g / n);
            o[2] = (unsigned char)(b / n);
        }
    }
}

/* RGB888 -> tiled RGB565. The 3DS stores textures as 8x8 tiles with Morton order inside
 * each tile; this index matches the font upload in imgui_sw.cpp, the only other place in
 * the tree that tiles anything. It is an element index, so for 16bpp it applies to a u16*
 * unchanged. Verified off-device to be a bijection over [0, 8192) for 128x64.
 * Padding past the image replicates the edge pixel so GPU_LINEAR never picks up
 * uninitialised texels at the u = 110/128 and v = 1 - 62/64 seams. */
static void thumb_swizzle_rgb565(const unsigned char *src, int srcW, int srcH, u16 *dst) {
    for (u32 y = 0; y < THUMB_TEX_H; y++) {
        const u32 sy = (y < (u32)srcH) ? y : (u32)(srcH - 1);
        const unsigned char *row = src + (size_t)sy * (size_t)srcW * 3u;
        const u32 tileRow = ((y >> 3) * (THUMB_TEX_W >> 3)) << 6;
        const u32 yBits   = ((y & 1u) << 1) | ((y & 2u) << 2) | ((y & 4u) << 3);

        for (u32 x = 0; x < THUMB_TEX_W; x++) {
            const u32 sx = (x < (u32)srcW) ? x : (u32)(srcW - 1);
            const unsigned char *p = row + (size_t)sx * 3u;
            const u16 c = (u16)((((u32)p[0] & 0xF8u) << 8)
                              | (((u32)p[1] & 0xFCu) << 3)
                              |  ((u32)p[2] >> 3));
            dst[tileRow + ((x >> 3) << 6)
                + ((x & 1u) | ((x & 2u) << 1) | ((x & 4u) << 2) | yBits)] = c;
        }
    }
}

/* The card's 92x38 box is a wider aspect than the 110x62 cache image, so the display
 * needs its own crop. Avalonia's Stretch="UniformToFill" scales to cover and centres the
 * overflow; expressed as subtexture UVs that costs nothing at draw time. */
static void thumb_build_subtex() {
    const float sx = CONTENT_W / (float)THUMB_IMG_W;
    const float sy = THUMB_H   / (float)THUMB_IMG_H;
    const float s  = sx > sy ? sx : sy;

    /* The cover scale lands one axis exactly on the source size, and binary floating
     * point can overshoot it by an ulp -- which would push a UV outside [0,1]. */
    float visW = CONTENT_W / s;
    float visH = THUMB_H   / s;
    if (visW > (float)THUMB_IMG_W) visW = (float)THUMB_IMG_W;
    if (visH > (float)THUMB_IMG_H) visH = (float)THUMB_IMG_H;

    float x0 = ((float)THUMB_IMG_W - visW) * 0.5f;
    float y0 = ((float)THUMB_IMG_H - visH) * 0.5f;
    if (x0 < 0.0f) x0 = 0.0f;
    if (y0 < 0.0f) y0 = 0.0f;

    /* v is flipped relative to storage (imgui_sw.cpp uses the same 1 - y/h convention),
     * which is why the swizzle above writes rows top-first with no flip of its own. */
    g_thumbSubTex.width  = (u16)CONTENT_W;
    g_thumbSubTex.height = (u16)THUMB_H;
    g_thumbSubTex.left   =         x0          / (float)THUMB_TEX_W;
    g_thumbSubTex.right  =        (x0 + visW)  / (float)THUMB_TEX_W;
    g_thumbSubTex.top    = 1.0f -  y0          / (float)THUMB_TEX_H;
    g_thumbSubTex.bottom = 1.0f - (y0 + visH)  / (float)THUMB_TEX_H;
}

/* -------------------------------------------------------------------------- */
/*  Disk cache and download                                                   */
/* -------------------------------------------------------------------------- */

static void thumb_cache_path(int modId, char *out, size_t outLen) {
    snprintf(out, outLen, "%s%d.jpg", THUMBNAIL_CACHE_DIR, modId);
}

static bool thumb_read_file(const char *path, unsigned char **out, size_t *outLen) {
    SdPathGuard sd;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    unsigned char *buf = nullptr;
    long sz = 0;
    bool ok = false;

    if (fseek(f, 0, SEEK_END) == 0) {
        sz = ftell(f);
        if (sz > 0 && (size_t)sz <= THUMB_MAX_BYTES) {
            rewind(f);
            buf = (unsigned char*)malloc((size_t)sz);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) ok = true;
        }
    }
    fclose(f);

    if (!ok) { free(buf); return false; }
    *out    = buf;
    *outLen = (size_t)sz;
    return true;
}

static bool thumb_write_atomic(const char *path, const void *data, size_t len) {
    SdPathGuard sd;

    char tmp[THUMB_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    const bool wrote  = (fwrite(data, 1, len, f) == len);
    const bool closed = (fclose(f) == 0);
    if (!wrote || !closed) { unlink(tmp); return false; }

    /* rename() on the 3DS maps to FSUSER_RenameFile, which fails if the target exists --
     * there is no File.Move(overwrite: true) equivalent. */
    unlink(path);
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

struct ThumbDl { unsigned char *data; size_t len; size_t cap; };

static size_t thumb_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    ThumbDl *dl = static_cast<ThumbDl*>(userp);
    const size_t total = size * nmemb;
    if (total == 0) return 0;
    if (dl->len + total > THUMB_MAX_BYTES) return 0;   /* short write aborts the transfer */

    if (dl->len + total > dl->cap) {
        size_t cap = dl->cap ? dl->cap : 32768;
        while (cap < dl->len + total) cap *= 2;
        unsigned char *nb = (unsigned char*)realloc(dl->data, cap);
        if (!nb) return 0;
        dl->data = nb;
        dl->cap  = cap;
    }
    memcpy(dl->data + dl->len, contents, total);
    dl->len += total;
    return total;
}

/* Lets a shutdown interrupt an in-flight download instead of waiting out the timeout. */
static int thumb_abort_cb(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_thumbQuit.load() ? 1 : 0;
}

static void thumb_curl_configure(CURL *curl) {
    curl_configure(curl);
    /* curl_configure attaches g_curlShare, which fetch_thread_func destroys the moment the
     * listing finishes; a thumbnail handle must never inherit that pointer. */
    curl_easy_setopt(curl, CURLOPT_SHARE, nullptr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, thumb_write_cb);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, nullptr);  /* JPEG is already compressed */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, thumb_abort_cb);
}

/* Disk hit, else download; then crop, scale, persist, and tile into `outSwizzled`.
 * Runs entirely on a worker thread and touches no GPU or browser state. */
static bool thumb_produce(CURL *curl, int modId, const char *url, u16 *outSwizzled) {
    char           path[THUMB_PATH_MAX];
    RawImage       img      = { nullptr, 0, 0 };
    ThumbDl        dl       = { nullptr, 0, 0 };
    unsigned char *file     = nullptr;
    size_t         fileLen  = 0;
    unsigned char *scaled   = nullptr;
    unsigned char *jpg      = nullptr;
    unsigned long  jpgLen   = 0;
    bool           fromDisk = false;
    bool           ok       = false;
    long           code     = 0;

    thumb_cache_path(modId, path, sizeof(path));

    const bool haveFile = thumb_read_file(path, &file, &fileLen);

    if (haveFile) {
        if (thumb_jpeg_decode(file, fileLen, &img)) {
            fromDisk = true;
        } else {
            /* Corrupt cache entry: drop it and refetch, as the original's catch does. */
            {
                SdPathGuard sd;
                unlink(path);
            }
        }
        free(file);
        file = nullptr;
    }

    if (!fromDisk) {
        if (!curl || url[0] == '\0' || g_thumbQuit.load()) goto done;

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dl);
        if (curl_easy_perform(curl) != CURLE_OK) goto done;
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) != CURLE_OK) goto done;
        if (code < 200 || code >= 300 || dl.len < 3) goto done;

        /* JPEG only. An HTML error page or anything else is simply "no thumbnail". */
        if (dl.data[0] != 0xFF || dl.data[1] != 0xD8 || dl.data[2] != 0xFF) goto done;
        if (!thumb_jpeg_decode(dl.data, dl.len, &img)) goto done;
    }

    scaled = (unsigned char*)malloc((size_t)THUMB_IMG_W * (size_t)THUMB_IMG_H * 3u);
    if (!scaled) goto done;
    thumb_crop_resize(&img, scaled);

    /* A disk hit is already encoded; only a fresh download needs writing back. */
    if (!fromDisk && thumb_jpeg_encode(scaled, &jpg, &jpgLen)) {
        thumb_write_atomic(path, jpg, (size_t)jpgLen);
    }

    thumb_swizzle_rgb565(scaled, THUMB_IMG_W, THUMB_IMG_H, outSwizzled);
    /* This thread dirtied the lines, so it flushes them; the main thread then needs no
     * C3D_TexFlush when it adopts the buffer. */
    GSPGPU_FlushDataCache(outSwizzled, THUMB_TEX_BYTES);
    ok = true;

done:
    free(img.rgb);
    free(scaled);
    free(jpg);
    free(dl.data);
    free(file);
    return ok;
}

/* -------------------------------------------------------------------------- */
/*  Pool bookkeeping (main thread unless noted)                               */
/* -------------------------------------------------------------------------- */

static bool thumb_is_wanted(int id) {
    for (int i = 0; i < g_thumbWantN; i++) if (g_thumbWant[i] == id) return true;
    return false;
}

static bool thumb_resident(int id) {
    for (int i = 0; i < THUMB_SLOTS; i++) if (g_thumbSlot[i].modId == id) return true;
    return false;
}

static bool thumb_failed(int id) {
    for (int i = 0; i < THUMB_FAIL_RING; i++) if (g_thumbFail[i] == id) return true;
    return false;
}

static void thumb_mark_failed(int id) {
    if (id == 0 || thumb_failed(id)) return;
    g_thumbFail[g_thumbFailPos] = id;
    g_thumbFailPos = (g_thumbFailPos + 1) % THUMB_FAIL_RING;
}

static bool thumb_inflight(int id) {
    for (int i = 0; i < THUMB_WORKERS; i++) if (g_thumbFlight[i] == id) return true;
    return false;
}

/* Never evicts something currently on screen, which is what keeps a slot from being
 * recycled out from under a draw. Guaranteed to find a victim while THUMB_SLOTS > 12. */
static ThumbSlot *thumb_pick_slot() {
    for (int i = 0; i < THUMB_SLOTS; i++) {
        if (g_thumbSlot[i].modId == 0) return &g_thumbSlot[i];
    }
    ThumbSlot *best = nullptr;
    for (int i = 0; i < THUMB_SLOTS; i++) {
        ThumbSlot &s = g_thumbSlot[i];
        if (thumb_is_wanted(s.modId)) continue;
        if (!best || s.lastUsed < best->lastUsed) best = &s;
    }
    return best;
}

static void thumb_rebuild_want() {
    g_thumbWantN = 0;
    const int n = visible_count();
    for (int i = 0; i < n && g_thumbWantN < CARDS_PER_PAGE; i++) {
        const ModData &m = g_mods[(size_t)(g_winStart + i)];
        if (m.Id != 0 && !m.ThumbnailUrl.empty()) g_thumbWant[g_thumbWantN++] = m.Id;
    }
}

/* Caller holds g_thumbLock. Push order is priority order: the grid reads left-to-right,
 * top-to-bottom, so that is the order the images fill in. */
static void thumb_rebuild_queue() {
    g_thumbQueueN = 0;
    const int n = visible_count();
    for (int i = 0; i < n && g_thumbQueueN < THUMB_QUEUE; i++) {
        const ModData &m = g_mods[(size_t)(g_winStart + i)];
        if (m.Id == 0 || m.ThumbnailUrl.empty()) continue;
        if (m.ThumbnailUrl.size() >= THUMB_URL_MAX) continue;
        if (thumb_resident(m.Id) || thumb_failed(m.Id) || thumb_inflight(m.Id)) continue;

        ThumbReq &r = g_thumbQueue[g_thumbQueueN++];
        r.id = m.Id;
        memcpy(r.url, m.ThumbnailUrl.c_str(), m.ThumbnailUrl.size() + 1);
    }
}

static void thumb_worker(void *arg) {
    const int wi  = (int)(uintptr_t)arg;
    ThumbPub &pub = g_thumbPub[wi];

    CURL *curl = curl_easy_init();
    if (curl) thumb_curl_configure(curl);

    for (;;) {
        /* Armed before the predicate is tested: clearing after a wait could swallow a
         * signal raised in between and park this worker on a non-empty queue. */
        LightEvent_Clear(&g_thumbWake);

        ThumbReq req;
        req.id     = 0;
        req.url[0] = '\0';

        LightLock_Lock(&g_thumbLock);
        if (!g_thumbQuit.load() && g_thumbQueueN > 0) {
            req = g_thumbQueue[0];
            for (int i = 1; i < g_thumbQueueN; i++) g_thumbQueue[i - 1] = g_thumbQueue[i];
            g_thumbQueueN--;
            g_thumbFlight[wi] = req.id;      /* claimed in the same critical section */
        }
        LightLock_Unlock(&g_thumbLock);

        if (g_thumbQuit.load()) break;
        if (req.id == 0) {
            /* g_thumbWake is shared and sticky, so another worker's Clear could in
             * principle cost this one a wake-up; the timeout bounds that. */
            LightEvent_WaitTimeout(&g_thumbWake, THUMB_IDLE_WAIT_NS);
            continue;
        }

        const bool produced = thumb_produce(curl, req.id, req.url, pub.pixels);
        if (g_thumbQuit.load()) break;

        LightEvent_Clear(&pub.ack);          /* before publishing: no lost signal */
        LightLock_Lock(&g_thumbLock);
        pub.modId = req.id;
        pub.ok    = produced;
        pub.ready = true;
        LightLock_Unlock(&g_thumbLock);

        /* Wait for the main thread to take pub.pixels. Polling the real condition on a
         * timeout rather than blocking on the event outright: a shutdown that signalled
         * between the Clear above and here would otherwise park this thread forever, and
         * threadJoin would never return. */
        for (;;) {
            LightEvent_WaitTimeout(&pub.ack, THUMB_IDLE_WAIT_NS);

            LightLock_Lock(&g_thumbLock);
            const bool taken = !pub.ready;
            LightLock_Unlock(&g_thumbLock);

            if (taken || g_thumbQuit.load()) break;
        }
        if (g_thumbQuit.load()) break;
    }

    LightLock_Lock(&g_thumbLock);
    g_thumbFlight[wi] = 0;
    LightLock_Unlock(&g_thumbLock);

    if (curl) curl_easy_cleanup(curl);
}

/* Called immediately after C3D_FrameBegin(C3D_FRAME_SYNCDRAW). That matters: the previous
 * frame's command list still holds the old buffer address, and SYNCDRAW has just waited
 * for that queue to retire, so nothing is reading the buffer handed back as scratch. */
static void thumbs_tick() {
    if (!g_thumbReady) return;
    g_thumbFrame++;

    LightLock_Lock(&g_thumbLock);
    thumb_rebuild_want();

    for (int w = 0; w < THUMB_WORKERS; w++) {
        ThumbPub &p = g_thumbPub[w];
        if (!p.ready) continue;

        if (!p.ok) {
            thumb_mark_failed(p.modId);
        } else if (thumb_is_wanted(p.modId) && !thumb_resident(p.modId)) {
            ThumbSlot *s = thumb_pick_slot();
            if (s) {
                /* Zero-copy handoff, and the buffer count is conserved exactly. */
                u16 *old    = static_cast<u16*>(s->tex.data);
                s->tex.data = p.pixels;
                p.pixels    = old;
                s->modId    = p.modId;
                s->lastUsed = g_thumbFrame;
            }
        }
        /* else: scrolled away while it loaded -- drop it and recycle the buffer. */

        g_thumbFlight[w] = 0;
        p.ready = false;
        LightEvent_Signal(&p.ack);
    }

    thumb_rebuild_queue();
    const bool haveWork = (g_thumbQueueN > 0);
    LightLock_Unlock(&g_thumbLock);

    if (haveWork) LightEvent_Signal(&g_thumbWake);
}

static C3D_Tex *thumb_for_mod(int modId) {
    if (!g_thumbReady || modId == 0) return nullptr;
    for (int i = 0; i < THUMB_SLOTS; i++) {
        if (g_thumbSlot[i].modId == modId) {
            g_thumbSlot[i].lastUsed = g_thumbFrame;
            return &g_thumbSlot[i].tex;
        }
    }
    return nullptr;
}

static bool thumbs_init() {
    if (g_thumbReady) return true;

    mkdir_p(THUMBNAIL_CACHE_DIR);
    thumb_build_subtex();

    LightLock_Init(&g_thumbLock);
    LightEvent_Init(&g_thumbWake, RESET_STICKY);

    /* All 16 textures are the same size and live for the whole session, so this is the
     * only place any of them is allocated -- publishing later just swaps pointers. */
    int slots = 0, pubs = 0;
    for (; slots < THUMB_SLOTS; slots++) {
        ThumbSlot &s = g_thumbSlot[slots];
        if (!C3D_TexInit(&s.tex, THUMB_TEX_W, THUMB_TEX_H, GPU_RGB565)) break;
        C3D_TexSetFilter(&s.tex, GPU_LINEAR, GPU_LINEAR);
        memset(s.tex.data, 0, THUMB_TEX_BYTES);
        C3D_TexFlush(&s.tex);
        s.modId    = 0;
        s.lastUsed = 0;
    }
    if (slots == THUMB_SLOTS) {
        for (; pubs < THUMB_WORKERS; pubs++) {
            ThumbPub &p = g_thumbPub[pubs];
            p.pixels = static_cast<u16*>(linearAlloc(THUMB_TEX_BYTES));
            if (!p.pixels) break;
            LightEvent_Init(&p.ack, RESET_STICKY);
            p.modId = 0;
            p.ready = false;
            p.ok    = false;
            g_thumbFlight[pubs] = 0;
        }
    }

    if (slots != THUMB_SLOTS || pubs != THUMB_WORKERS) {
        for (int i = 0; i < slots; i++) C3D_TexDelete(&g_thumbSlot[i].tex);
        for (int i = 0; i < pubs;  i++) linearFree(g_thumbPub[i].pixels);
        memset(g_thumbSlot, 0, sizeof(g_thumbSlot));
        memset(g_thumbPub,  0, sizeof(g_thumbPub));
        return false;   /* browsing still works, cards just keep the placeholder */
    }

    g_thumbQuit.store(false);
    g_thumbReady = true;

    for (int w = 0; w < THUMB_WORKERS; w++) {
        g_thumbThread[w] = threadCreate(thumb_worker, (void*)(uintptr_t)w,
                                        WORKER_STACK_SIZE, WORKER_PRIORITY, -2, false);
    }
    return true;
}

static void thumbs_shutdown() {
    if (!g_thumbReady) return;
    g_thumbReady = false;

    g_thumbQuit.store(true);
    LightEvent_Signal(&g_thumbWake);
    for (int w = 0; w < THUMB_WORKERS; w++) LightEvent_Signal(&g_thumbPub[w].ack);

    for (int w = 0; w < THUMB_WORKERS; w++) {
        if (!g_thumbThread[w]) continue;
        threadJoin(g_thumbThread[w], U64_MAX);
        threadFree(g_thumbThread[w]);
        g_thumbThread[w] = nullptr;
    }
    for (int w = 0; w < THUMB_WORKERS; w++) {
        if (!g_thumbPub[w].pixels) continue;
        linearFree(g_thumbPub[w].pixels);
        g_thumbPub[w].pixels = nullptr;
    }
    for (int i = 0; i < THUMB_SLOTS; i++) {
        C3D_TexDelete(&g_thumbSlot[i].tex);
        g_thumbSlot[i].modId = 0;
    }
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

    /* Started here rather than earlier so the fetch pool has already released g_curlShare. */
    thumbs_init();
    install_init();
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

    /* Resident thumbnail, or the placeholder while it loads / if it never arrives --
     * the original shows nothing in both of those cases too. */
    C3D_Tex *thumb = thumb_for_mod(mod.Id);
    if (thumb) {
        C2D_Image thumbImg = { thumb, &g_thumbSubTex };
        C2D_DrawImageAt(thumbImg, cx, cy, 0.0f, nullptr, 1.0f, 1.0f);
    } else {
        C2D_DrawRectSolid(cx, cy, 0.0f, CONTENT_W, THUMB_H, CLR_THUMB);
    }

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

    const bool busy = install_busy();

    /* --- Action button ------------------------------------------------------- */
    {
        const ModAction   action = current_action();
        /* Matches ActionButtonText = "Working..." upstream, but with progress: on a 3DS
         * this takes minutes, not the second it takes on a desktop. */
        const std::string label  = busy ? install_progress_label()
                                        : action_label(action, selected_mod());
        const ImVec4&     fg     = (action == ACTION_UPDATE) ? IM_AMBER : IM_GOLD;

        ImGui::BeginDisabled(busy || (action != ACTION_INSTALL && action != ACTION_UPDATE));
        if (wrapped_button("##action", label, ACTION_BTN_Y, ACTION_BTN_H,
                           IM_BTN_BG, IM_BTN_HOT, fg, 2.0f)) {
            do_action();
        }
        ImGui::EndDisabled();
    }

    /* --- Progress bar, in the gap between the buttons ------------------------- */
    if (busy) {
        const int   pct = g_instPct.load();
        ImDrawList *dl  = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(BTN_X, PROG_BAR_Y),
                          ImVec2(BTN_X + BTN_W, PROG_BAR_Y + PROG_BAR_H),
                          IM_COL32(0x2A, 0x3B, 0x47, 0xFF));
        if (pct > 0) {   /* pct < 0 means unknown: leave the track empty rather than lie */
            const float w = BTN_W * (float)(pct < 100 ? pct : 100) / 100.0f;
            dl->AddRectFilled(ImVec2(BTN_X, PROG_BAR_Y),
                              ImVec2(BTN_X + w, PROG_BAR_Y + PROG_BAR_H),
                              ImGui::GetColorU32(IM_GOLD));
        }
    }

    /* --- Uninstall button ---------------------------------------------------- */
    {
        /* Re-read: installing above may have re-sorted the page under us. The original
         * hides this button outright; here it stays put and greys out instead. */
        const ModData *mod = selected_mod();
        const bool can_uninstall = !busy && mod && g_installed.count(mod->Id) != 0;

        ImGui::BeginDisabled(!can_uninstall);
        if (wrapped_button("##uninstall", "Uninstall", UNINST_BTN_Y, UNINST_BTN_H,
                           IM_UNINST_BG, IM_UNINST_HOT, IM_UNINST_FG, 1.0f)) {
            do_uninstall();
        }
        ImGui::EndDisabled();
    }

    /* --- Position readout ----------------------------------------------------- */
    {
        char buf[48];
        const int total = mod_count();
        const int cur   = (total > 0 && selected_mod()) ? g_winStart + g_selIdx + 1 : 0;
        snprintf(buf, sizeof(buf), "%d / %d", cur, total);
        imgui_text_centered(buf, IM_AUTHOR, COUNTER_Y);
    }

    /* --- Error message, or the button hints ----------------------------------- */
    if (!g_installMsg.empty()) {
        /* The hints are exactly what the user does not need while reading an error, so
         * the message takes their space instead of needing geometry of its own. */
        std::vector<std::string> lines;
        wrap_lines(g_installMsg, BOT_W - 16.0f, MSG_MAX_LINES, lines);

        float y = MSG_LINE_Y;
        for (size_t i = 0; i < lines.size(); i++) {
            imgui_text_centered(lines[i].c_str(), IM_ERROR, y);
            y += ImGui::GetTextLineHeight() + 2.0f;
        }
    } else {
        imgui_text_centered("[A] Install   [B] Uninstall   [X] Sort", IM_AUTHOR, HINT1_Y);
        imgui_text_centered(busy ? "[B] Cancel   [START] Exit" : "[START] Exit",
                            IM_AUTHOR, HINT2_Y);
    }

    ImGui::End();
}

static void handle_browse_input(u32 navKeys, u32 kDown) {
    handle_nav(navKeys);
    if (navKeys) g_installMsg.clear();

    /* The keys are an input route independent of the buttons, so they need their own
     * gate -- ImGui::BeginDisabled only blocks touch. */
    if (install_busy()) {
        /* B cancels rather than uninstalls while working: the uninstall button is
         * disabled anyway, and a stalled download would otherwise have no escape short
         * of quitting the app. */
        if (kDown & KEY_B) install_cancel();
        /* Sorting stays live -- it only reorders g_mods, and the running request was
         * copied by value and is keyed by mod Id. */
        if (kDown & KEY_X) set_sort_mode(!g_sortByName);
        return;
    }

    if (kDown & KEY_A) do_action();
    if (kDown & KEY_B) do_uninstall();
    if (kDown & KEY_X) set_sort_mode(!g_sortByName);
}

/* -------------------------------------------------------------------------- */
/*  Entry point                                                               */
/* -------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    LightLock_Init(&status_lock);
    RecursiveLock_Init(&g_sdPathLock);   /* before anything can touch the filesystem */

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
            /* Settle any finished install first, so input this frame sees the new state
             * and both screens render the same one. */
            install_tick();
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

        /* Adopt any freshly decoded thumbnails. Must sit here: SYNCDRAW has just waited
         * out the previous frame's queue, so no buffer being recycled is still in use. */
        thumbs_tick();

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

    /* Before curl_global_cleanup (the workers hold easy handles) and before C2D_Fini
     * (the slots hold live textures). */
    thumbs_shutdown();
    install_shutdown();

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