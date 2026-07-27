#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <curl/curl.h>
#include <json-c/json.h>

#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <malloc.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <cerrno>
#include <cstddef>
#include <unordered_map>

#include "imgui/imgui.h"
#include "imgui/imgui_sw.h"

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
static constexpr u64    INTER_CATEGORY_DELAY_NS = 200000000ULL;
static constexpr u64    BATCH_ERROR_SLEEP_NS    = 700000000ULL;
static constexpr u64    POST_FETCH_DELAY_NS     = 1500000000ULL;

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

/* -------------------------------------------------------------------------- */
/*  Data Structures & Globals for Rendering                                   */
/* -------------------------------------------------------------------------- */

struct ModData {
    int         Id             = 0;
    std::string Name;
    std::string Author;
    std::string ThumbnailUrl;
    std::string LatestFileUrl;
    int64_t     LatestFileDate = 0;
    std::string LatestFileName;
};

struct FetchResult {
    std::string data;
    long        response_code = 0;
    bool        success       = false;
};

static LightLock status_lock;
static std::string g_statusText = "Initializing...";
static bool g_fetchDone = false;
static bool g_fetchSuccess = false;

/* -------------------------------------------------------------------------- */
/*  Logging (Thread-Safe)                                                     */
/* -------------------------------------------------------------------------- */

static void print_status(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    LightLock_Lock(&status_lock);
    g_statusText = buffer;
    LightLock_Unlock(&status_lock);
}

/* -------------------------------------------------------------------------- */
/*  Filesystem helpers                                                        */
/* -------------------------------------------------------------------------- */

static int rmrf(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;

    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        std::string fullpath = std::string(path);
        if (!fullpath.empty() && fullpath.back() != '/') fullpath += "/";
        fullpath += entry->d_name;

        struct stat st;
        if (lstat(fullpath.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
                if (rmrf(fullpath.c_str()) != 0) { closedir(d); return -1; }
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
    return rmdir(path) == 0 ? 0 : -1;
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

    if (total > 0 && result->size() + total > MAX_RESPONSE_SIZE) {
        return 0;  /* Signal unrecoverable error to libcurl */
    }

    result->append(static_cast<char*>(contents), total);
    return total;
}

/* -------------------------------------------------------------------------- */
/*  HTTP / network                                                            */
/* -------------------------------------------------------------------------- */

static FetchResult curl_get(CURL *curl, const std::string& url) {
    FetchResult result;
    if (!curl) return result;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.data);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE_PATH);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.response_code) == CURLE_OK) {
            result.success = (result.response_code >= 200 && result.response_code < 300);
        }
    } else {
        print_status("cURL Error: %s", curl_easy_strerror(res));
    }

    if (!result.success) {
        result.data.clear();
    }

    return result;
}

static FetchResult curl_get_retry(CURL *curl, const std::string& url, int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (attempt > 0) {
            svcSleepThread(RETRY_BASE_DELAY_NS << (attempt - 1));
        }
        FetchResult result = curl_get(curl, url);
        if (result.success) return result;
    }
    return FetchResult{};
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

static bool write_mods_json(const std::string& filename, const std::vector<ModData>& mods) {
    json_object *jarray = json_object_new_array();
    if (!jarray) return false;

    for (const auto& mod : mods) {
        json_object *jobj = json_object_new_object();
        if (!jobj) { json_object_put(jarray); return false; }

        json_object_object_add(jobj, "Id",             json_object_new_int(mod.Id));
        json_object_object_add(jobj, "Name",           json_object_new_string(mod.Name.c_str()));
        json_object_object_add(jobj, "Author",         json_object_new_string(mod.Author.c_str()));
        json_object_object_add(jobj, "ThumbnailUrl",   json_object_new_string(mod.ThumbnailUrl.c_str()));
        json_object_object_add(jobj, "LatestFileUrl",  json_object_new_string(mod.LatestFileUrl.c_str()));
        json_object_object_add(jobj, "LatestFileDate", json_object_new_int64(mod.LatestFileDate));
        json_object_object_add(jobj, "LatestFileName", json_object_new_string(mod.LatestFileName.c_str()));

        json_object_array_add(jarray, jobj);
    }

    int res = json_object_to_file_ext(filename.c_str(), jarray, JSON_C_TO_STRING_NOSLASHESCAPE);
    json_object_put(jarray);
    return (res >= 0);
}

/* -------------------------------------------------------------------------- */
/*  GameBanana fetching                                                       */
/* -------------------------------------------------------------------------- */

static void fetch_category(CURL *curl, int cat_id, std::vector<ModData>& out_mods) {
    std::string url = std::string(API_V10_INDEX)
        + "?_nPage=1&_nPerpage=50&_aFilters[Generic_Category]="
        + std::to_string(cat_id);

    FetchResult result = curl_get_retry(curl, url, MAX_FETCH_ATTEMPTS);
    if (!result.success) {
        print_status("Category %d failed after retries, skipping", cat_id);
        svcSleepThread(700000000ULL);
        return;
    }

    json_object *root = json_tokener_parse(result.data.c_str());
    if (!root) return;

    json_object *records;
    if (!json_object_object_get_ex(root, "_aRecords", &records)
        || json_object_get_type(records) != json_type_array) {
        json_object_put(root);
        return;
    }

    int len = json_object_array_length(records);
    if (len <= 0) { json_object_put(root); return; }

    for (int i = 0; i < len; i++) {
        json_object *record = json_object_array_get_idx(records, i);
        if (!record || json_object_get_type(record) != json_type_object) continue;

        int id = get_json_int(record, "_idRow");
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

        out_mods.push_back(std::move(mod));
    }
    json_object_put(root);
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
                mod.LatestFileDate = max_ts;
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

static void fetch_core_data(CURL *curl, std::vector<ModData>& mods) {
    print_status("Fetching core data for %zu items...", mods.size());
    const size_t BATCH_SIZE = 20;

    for (size_t i = 0; i < mods.size(); i += BATCH_SIZE) {
        std::string url = std::string(API_CORE_DATA) + "?";
        size_t items_in_batch = 0;
        std::vector<ModData*> batch_ptrs;
        batch_ptrs.reserve(BATCH_SIZE);

        for (size_t j = 0; j < BATCH_SIZE && (i + j) < mods.size(); j++) {
            if (!mods[i + j].LatestFileUrl.empty()) continue;

            url += "itemtype[]=Mod&itemid[]=" + std::to_string(mods[i + j].Id)
                 + "&fields[]=Files().aFiles()&";
            batch_ptrs.push_back(&mods[i + j]);
            items_in_batch++;
        }

        if (items_in_batch == 0) continue;
        if (url.back() == '&') url.pop_back();

        print_status("[%zu/%zu] Fetching batch of %zu mods...",
                     std::min(i + items_in_batch, mods.size()),
                     mods.size(), items_in_batch);

        FetchResult result = curl_get_retry(curl, url, MAX_FETCH_ATTEMPTS);
        if (!result.success) {
            print_status("Batch fetch failed after retries, continuing...");
            svcSleepThread(BATCH_ERROR_SLEEP_NS);
            continue;
        }

        json_object *root = json_tokener_parse(result.data.c_str());
        if (!root || json_object_get_type(root) != json_type_array) {
            if (root) json_object_put(root);
            print_status("Batch JSON parse failed, continuing...");
            continue;
        }

        int arr_len = json_object_array_length(root);
        if (arr_len <= 0) { json_object_put(root); continue; }

        for (int k = 0; k < arr_len && k < static_cast<int>(batch_ptrs.size()); k++) {
            json_object *item = json_object_array_get_idx(root, k);
            if (!item) continue;
            parse_latest_file(item, *batch_ptrs[k]);
        }
        json_object_put(root);
    }
}

/* -------------------------------------------------------------------------- */
/*  Background Fetch Thread                                                   */
/* -------------------------------------------------------------------------- */

static void fetch_thread_func(void* arg) {
    (void)arg;
    CURL *curl = curl_easy_init();
    bool fetch_succeeded = false;

    if (!curl) {
        print_status("curl_easy_init failed!");
        goto exit_thread;
    }

    rmrf(CACHE_DIR);
    init_paths();

    print_status("Fetching %zu categories...", NUM_CATEGORIES);

    {
        std::vector<ModData> all_mods;

        for (size_t i = 0; i < NUM_CATEGORIES; i++) {
            print_status("Fetching category %zu/%zu...", i + 1, NUM_CATEGORIES);
            fetch_category(curl, CATEGORIES[i], all_mods);
            if (i + 1 < NUM_CATEGORIES) {
                svcSleepThread(INTER_CATEGORY_DELAY_NS);
            }
        }

        if (!all_mods.empty()) {
            deduplicate_mods(all_mods);
            fetch_core_data(curl, all_mods);

            size_t enriched = 0;
            for (const auto& m : all_mods) if (!m.LatestFileUrl.empty()) enriched++;

            if (!write_mods_json(MOD_LIST_FILE, all_mods)) {
                print_status("Error saving mod list!");
            }

            std::sort(all_mods.begin(), all_mods.end(), [](const ModData& a, const ModData& b) { return a.Name < b.Name; });
            write_mods_json(BY_NAME_FILE, all_mods);

            std::sort(all_mods.begin(), all_mods.end(), [](const ModData& a, const ModData& b) { return a.LatestFileDate > b.LatestFileDate; });
            write_mods_json(BY_UPDATED_FILE, all_mods);

            fetch_succeeded = (enriched > 0);
            print_status("Done! Enriched %zu/%zu mods.", enriched, all_mods.size());
        } else {
            print_status("Failed to fetch any mods!");
        }
    }

    svcSleepThread(POST_FETCH_DELAY_NS);
    print_status("Press START to exit.");

    curl_easy_cleanup(curl);

exit_thread:
    LightLock_Lock(&status_lock);
    g_fetchSuccess = fetch_succeeded;
    g_fetchDone = true;
    LightLock_Unlock(&status_lock);
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

    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(320.0f, 240.0f);
    imgui_sw::bind_imgui_painting();
    imgui_sw::SwOptions sw_options;
    imgui_sw::make_style_fast();

    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0x15 / 255.0f, 0x1D / 255.0f, 0x23 / 255.0f, 1.0f);
    style.WindowRounding = 0.0f;
    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.WindowBorderSize = 0.0f;

    // Screen color: #151d23 | Text color: #ABA022
    u32 clrClear = C2D_Color32(0x15, 0x1D, 0x23, 0xFF);
    u32 clrText = C2D_Color32(0xAB, 0xA0, 0x22, 0xFF);

    Thread fetchThread = nullptr;

    bool romfs_mounted = R_SUCCEEDED(romfsInit());
    if (!romfs_mounted) {
        print_status("romfsInit failed - no CA bundle!");
        g_fetchDone = true;
        goto main_loop;
    }

    {
        struct stat sb;
        if (stat("romfs:/cacert.pem", &sb) != 0) {
            print_status("Fatal: CA bundle missing from ROMFS");
            g_fetchDone = true;
            goto main_loop;
        }
    }

    socBuffer = static_cast<u32*>(memalign(SOC_ALIGN, SOC_BUFFERSIZE));
    if (!socBuffer || R_FAILED(socInit(socBuffer, SOC_BUFFERSIZE))) {
        print_status("socInit failed!");
        g_fetchDone = true;
        goto main_loop;
    }

    if (R_FAILED(sslcInit(0))) {
        print_status("sslcInit failed!");
        g_fetchDone = true;
        goto main_loop;
    }

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        print_status("curl_global_init failed!");
        g_fetchDone = true;
        goto main_loop;
    }

    // Launch background thread so UI doesn't freeze
    fetchThread = threadCreate(fetch_thread_func, NULL, 64 * 1024, 0x3F, -2, true);

main_loop:
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        LightLock_Lock(&status_lock);
        bool done = g_fetchDone;
        std::string currentText = g_statusText;
        LightLock_Unlock(&status_lock);

        if (done && (kDown & KEY_START)) break;

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        // Top screen: just clear it to background color
        C2D_TargetClear(top, clrClear);
        C2D_SceneBegin(top);

        // Bottom screen: Render the loading text centered via ImGui
        C2D_TargetClear(bottom, clrClear);
        C2D_SceneBegin(bottom);

        ImGui::NewFrame();
        {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(320.0f, 240.0f));
            ImGui::Begin("Status", nullptr,
                ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoSavedSettings);

            ImVec2 textSize = ImGui::CalcTextSize(currentText.c_str());
            float x = (320.0f - textSize.x) / 2.0f;
            float y = (240.0f - textSize.y) / 2.0f;

            ImVec4 textColor = ImVec4(
                ((clrText >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
                ((clrText >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                ((clrText >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f,
                ((clrText >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f
            );

            ImGui::SetCursorPos(ImVec2(x, y));
            ImGui::TextColored(textColor, "%s", currentText.c_str());

            ImGui::End();
        }
        ImGui::Render();

        C2D_Prepare();
        imgui_sw::paint_imgui(320, 240, sw_options);
        C2D_Flush();

        C3D_FrameEnd(0);
    }

    if (fetchThread) {
        threadJoin(fetchThread, U64_MAX);
        threadFree(fetchThread);
    }

    curl_global_cleanup();
    sslcExit();
    if (socBuffer) {
        socExit();
        free(socBuffer);
    }
    if (romfs_mounted) romfsExit();

    C2D_Fini();
    C3D_Fini();
    gfxExit();

    return g_fetchSuccess ? 0 : 1;
}