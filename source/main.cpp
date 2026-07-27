#include <3ds.h>
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

#define CONSOLE_COLS 40
#define CONSOLE_ROWS 30

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
/*  Data Structures                                                           */
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

/* -------------------------------------------------------------------------- */
/*  Forward declarations                                                      */
/* -------------------------------------------------------------------------- */

static void print_status(const char* format, ...);

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
                /* Special files: attempt removal, but don’t hard-fail */
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
                print_status("mkdir_p: failed to create ‘%s’ (errno=%d)", sub_path.c_str(), errno);
                return false;
            }
        }
    }
    if (mkdir(buf.c_str(), 0777) != 0 && errno != EEXIST) {
        print_status("mkdir_p: failed to create ‘%s’ (errno=%d)", path, errno);
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
        print_status("Response exceeds %zu bytes, aborting transfer", MAX_RESPONSE_SIZE);
        return 0;  /* Signal unrecoverable error to libcurl */
    }

    result->append(static_cast<char*>(contents), total);
    return total;
}

/* -------------------------------------------------------------------------- */
/*  Logging                                                                   */
/* -------------------------------------------------------------------------- */

static void print_status(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (len < 0) return;

    const int MAX_VISIBLE = CONSOLE_COLS - 3;
    if (len > MAX_VISIBLE && MAX_VISIBLE > 0) {
        memcpy(buffer + MAX_VISIBLE, "...", 3);
        buffer[MAX_VISIBLE + 3] = '\0';
        len = MAX_VISIBLE + 3;
    }

    int col = (CONSOLE_COLS - len) / 2 + 1;
    if (col < 1) col = 1;
    printf("\x1b[15;1H\x1b[2K\x1b[1m\x1b[15;%dH%s", col, buffer);
    fflush(stdout);
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

    std::sort(mods.begin(), mods.end(),
              [](const ModData& a, const ModData& b) { return a.Id < b.Id; });

    auto last = std::unique(mods.begin(), mods.end(),
                            [](const ModData& a, const ModData& b) {
                                return a.Id == b.Id;
                            });
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
/*  Entry point                                                               */
/* -------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    CURL *curl = nullptr;
    bool fetch_succeeded = false;

    gfxInitDefault();

    bool romfs_mounted = R_SUCCEEDED(romfsInit());

    PrintConsole topScreen, bottomScreen;
    consoleInit(GFX_TOP,    &topScreen);
    consoleInit(GFX_BOTTOM, &bottomScreen);

    consoleSelect(&topScreen);
    printf("\x1b[48;2;21;29;35m\x1b[2J");

    consoleSelect(&bottomScreen);
    printf("\x1b[38;2;171;160;34m\x1b[48;2;21;29;35m\x1b[2J");

    print_status("GamebananaFetcher 3DS Port (C++)");

    if (!romfs_mounted) {
        print_status("romfsInit failed - no CA bundle, cannot verify TLS!");
        svcSleepThread(2000000000ULL);
        goto exit_no_soc;
    }

    {
        struct stat sb;
        if (stat("romfs:/cacert.pem", &sb) != 0) {
            print_status("Fatal: CA bundle missing from ROMFS");
            svcSleepThread(2000000000ULL);
            goto exit_no_soc;
        }
    }

    socBuffer = static_cast<u32*>(memalign(SOC_ALIGN, SOC_BUFFERSIZE));
    if (!socBuffer) {
        print_status("Failed to allocate SOC buffer!");
        goto exit_no_soc;
    }
    if (R_FAILED(socInit(socBuffer, SOC_BUFFERSIZE))) {
        print_status("socInit failed!");
        goto exit_soc_fail;
    }

    if (R_FAILED(sslcInit(0))) {
        print_status("sslcInit failed!");
        goto exit_sslc_fail;
    }

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        print_status("curl_global_init failed!");
        goto exit_curl_fail;
    }

    curl = curl_easy_init();
    if (!curl) {
        print_status("curl_easy_init failed!");
        goto exit_curl_handle_fail;
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

            std::sort(all_mods.begin(), all_mods.end(),
                      [](const ModData& a, const ModData& b) { return a.Name < b.Name; });
            write_mods_json(BY_NAME_FILE, all_mods);

            std::sort(all_mods.begin(), all_mods.end(),
                      [](const ModData& a, const ModData& b) {
                          return a.LatestFileDate > b.LatestFileDate;
                      });
            write_mods_json(BY_UPDATED_FILE, all_mods);

            fetch_succeeded = (enriched > 0);
            print_status("Done! Enriched %zu/%zu mods.",
                         enriched, all_mods.size());
        } else {
            print_status("Failed to fetch any mods!");
        }
    }

    svcSleepThread(POST_FETCH_DELAY_NS);
    print_status("Press START to exit.");

    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
    }

    if (curl) {
        curl_easy_cleanup(curl);
        curl = nullptr;
    }

exit_curl_handle_fail:
    curl_global_cleanup();
exit_curl_fail:
    sslcExit();
exit_sslc_fail:
    socExit();
exit_soc_fail:
    free(socBuffer);
exit_no_soc:
    if (romfs_mounted) romfsExit();

    gfxExit();
    return fetch_succeeded ? 0 : 1;
}
