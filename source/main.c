#include <3ds.h>
#include <citro3d.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>
#include <ctype.h>

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000 // 1 MiB minimum allocation required by libnx/soc
static u32 *socBuffer = NULL;

static const char *USER_AGENT = "CTGP-7-Mod-Manager/3.0";
static const char *API_V10_INDEX = "https://gamebanana.com/apiv10/Mod/Index";
static const char *API_CORE_DATA = "https://api.gamebanana.com/Core/Item/Data";

static const char *CA_BUNDLE_PATH = "romfs:/cacert.pem";

#define MAX_RESPONSE_SIZE (512 * 1024)

#define MAX_FETCH_ATTEMPTS 4
#define RETRY_BASE_DELAY_NS 500000000ULL

static const int CATEGORIES[] = {35931, 10605, 35932, 35943, 35933, 35935, 35937, 35938, 35939, 35941, 35942, 35944, 35946, 35947, 35945, 35940, 35934, 35936};
static const int NUM_CATEGORIES = sizeof(CATEGORIES) / sizeof(CATEGORIES[0]);

static const bool log_to_file = true;

#define MAX_PATH 512
#define MAX_URL 2048

#define BASE_DIR "sdmc:/"
#define APP_DIR BASE_DIR "3ds/CTGP-7-Mod-Manager/"
#define CACHE_DIR APP_DIR "cache/"
#define LISTS_DIR CACHE_DIR "lists/"
#define THUMBNAIL_CACHE_DIR CACHE_DIR "images/"
#define CTGP7_DIR BASE_DIR "CTGP-7/MyStuff/Characters/"

#define STATE_FILE APP_DIR "installed_mods.json"
#define LOG_FILE APP_DIR "log.txt"
#define MOD_LIST_FILE LISTS_DIR "modlist.json"
#define BY_NAME_FILE LISTS_DIR "byname.json"
#define BY_UPDATED_FILE LISTS_DIR "byupdated.json"

typedef struct {
    int Id;
    char Name[256];
    char Author[128];
    char ThumbnailUrl[512];
    char LatestFileUrl[512];
    int64_t LatestFileDate;
    char LatestFileName[256];
} ModData;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
    long response_code;
    bool success;
} FetchResult;

// recursive delete because newlib does not have POSIX extensions
// Kagi Assistant, audited for errors
static int rmrf(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        perror("opendir");
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char fullpath[PATH_MAX + 256];
        const char *sep = "/";
        size_t len = strlen(path);
        if (len > 0 && path[len - 1] == '/') {
            sep = "";
        }

        int w_len = snprintf(fullpath, sizeof(fullpath), "%s%s%s", path, sep, entry->d_name);
        if (w_len < 0 || (size_t)w_len >= sizeof(fullpath)) continue;

        struct stat st;
        if (stat(fullpath, &st) == 0) {
            if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
                if (rmrf(fullpath) != 0) {
                    closedir(d);
                    return -1;
                }
            } else {
                if (unlink(fullpath) != 0) {
                    perror("unlink");
                    closedir(d);
                    return -1;
                }
            }
        } else if (errno != ENOENT) {
            perror(fullpath); // failing here is normal, but we print the path incase our str building is wrong
            closedir(d);
            return -1;
        }
    }

    closedir(d);

    if (rmdir(path) != 0) {
        perror("rmdir");
        return -1;
    }

    return 0;
}

static void mkdir_p(const char *path) {
    char buf[MAX_PATH];
    int len = snprintf(buf, sizeof(buf), "%s", path);
    if (len < 0 || (size_t)len >= sizeof(buf)) return;

    size_t prefix_len = strlen("sdmc:/");
    char *start = buf;
    if (strncmp(buf, "sdmc:/", prefix_len) == 0) {
        start = buf + prefix_len;
    }

    for (char *p = start; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
                return;
            }
            *p = '/';
        }
    }
    if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
        return;
    }
}

static void init_paths(void) {
    mkdir_p(APP_DIR);
    mkdir_p(LISTS_DIR);
    mkdir_p(THUMBNAIL_CACHE_DIR);
    mkdir_p(CTGP7_DIR);
}

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    uint64_t total = (uint64_t)size * nmemb;
    FetchResult *result = (FetchResult *)userp;

    if (result->size + total + 1 > MAX_RESPONSE_SIZE) {
        return 0;
    }

    if (result->size + total + 1 > result->capacity) {
        size_t new_cap = result->capacity * 2;
        if (new_cap < result->size + total + 1) new_cap = result->size + total + 1;
        if (new_cap > MAX_RESPONSE_SIZE) new_cap = MAX_RESPONSE_SIZE;
        char *new_data = (char *)realloc(result->data, new_cap);
        if (!new_data) return 0;
        result->data = new_data;
        result->capacity = new_cap;
    }
    memcpy(result->data + result->size, contents, (size_t)total);
    result->size += (size_t)total;
    result->data[result->size] = '\0';
    return (size_t)total;
}

static void free_fetch_result(FetchResult *result);

static FetchResult curl_get(CURL *curl, const char *url) {
    FetchResult result;
    memset(&result, 0, sizeof(result));
    result.capacity = 4096;
    result.data = (char *)malloc(result.capacity);
    if (!result.data) return result;

    if (!curl) {
        free(result.data);
        memset(&result, 0, sizeof(result));
        return result;
    }

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
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
            if (result.response_code >= 200 && result.response_code < 300) {
                result.success = true;
            }
        }
    } else {
        printf("cURL Error: %s\n", curl_easy_strerror(res));
    }

    return result;
}

static FetchResult curl_get_retry(CURL *curl, const char *url, int max_attempts) {
    FetchResult result;
    memset(&result, 0, sizeof(result));

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (attempt > 0) {
            free_fetch_result(&result);
            svcSleepThread(RETRY_BASE_DELAY_NS << (attempt - 1));
        }
        result = curl_get(curl, url);
        if (result.success) return result;
    }
    return result;
}

static void free_fetch_result(FetchResult *result) {
    if (result->data) free(result->data);
    memset(result, 0, sizeof(*result));
}

static const char *get_json_string(json_object *obj, const char *key) {
    json_object *val;
    if (json_object_object_get_ex(obj, key, &val) && json_object_get_type(val) == json_type_string) {
        return json_object_get_string(val);
    }
    return NULL;
}

static int get_json_int(json_object *obj, const char *key) {
    json_object *val;
    if (json_object_object_get_ex(obj, key, &val)) {
        if (json_object_get_type(val) == json_type_int) {
            return json_object_get_int(val);
        } else if (json_object_get_type(val) == json_type_string) {
            return atoi(json_object_get_string(val));
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
            return strtoll(json_object_get_string(val), NULL, 10);
        }
    }
    return 0;
}

static void escape_json_string(const char *src, char *dst, int dst_size) {
    if (!src) { dst[0] = '\0'; return; }
    int di = 0;
    for (int i = 0; src[i] && di < dst_size - 2; i++) {
        char c = src[i];
        if (c == '"') { dst[di++] = '\\'; dst[di++] = '"'; }
        else if (c == '\\') { dst[di++] = '\\'; dst[di++] = '\\'; }
        else if (c == '\n') { dst[di++] = '\\'; dst[di++] = 'n'; }
        else if (c == '\r') { dst[di++] = '\\'; dst[di++] = 'r'; }
        else if (c == '\t') { dst[di++] = '\\'; dst[di++] = 't'; }
        else dst[di++] = c;
    }
    dst[di] = '\0';
}

static bool write_mods_json(const char *filename, ModData *mods, int count) {
    FILE *f = fopen(filename, "wb");
    if (!f) return false;

    fprintf(f, "[\n");
    for (int i = 0; i < count; i++) {
        char name_esc[512], author_esc[256], thumb_esc[512], url_esc[512], fname_esc[256];
        escape_json_string(mods[i].Name, name_esc, sizeof(name_esc));
        escape_json_string(mods[i].Author, author_esc, sizeof(author_esc));
        escape_json_string(mods[i].ThumbnailUrl, thumb_esc, sizeof(thumb_esc));
        escape_json_string(mods[i].LatestFileUrl, url_esc, sizeof(url_esc));
        escape_json_string(mods[i].LatestFileName, fname_esc, sizeof(fname_esc));

        fprintf(f, "  {\"Id\":%d,", mods[i].Id);
        fprintf(f, "\"Name\":\"%s\",", name_esc);
        fprintf(f, "\"Author\":\"%s\",", author_esc);
        fprintf(f, "\"ThumbnailUrl\":\"%s\",", thumb_esc);
        fprintf(f, "\"LatestFileUrl\":\"%s\",", url_esc);
        fprintf(f, "\"LatestFileDate\":%" PRId64 ",", mods[i].LatestFileDate);
        fprintf(f, "\"LatestFileName\":\"%s\"}", fname_esc);
        if (i < count - 1) fprintf(f, ",");
        fprintf(f, "\n");
    }
    fprintf(f, "]\n");
    fflush(f);

    if (ferror(f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static int compare_mods_by_name(const void *a, const void *b) {
    const ModData *ma = (const ModData *)a;
    const ModData *mb = (const ModData *)b;
    return strcmp(ma->Name, mb->Name);
}

static int compare_mods_by_updated(const void *a, const void *b) {
    const ModData *ma = (const ModData *)a;
    const ModData *mb = (const ModData *)b;
    if (ma->LatestFileDate < mb->LatestFileDate) return 1;
    if (ma->LatestFileDate > mb->LatestFileDate) return -1;
    return 0;
}

static int compare_mods_by_id(const void *a, const void *b) {
    int id_a = ((const ModData *)a)->Id;
    int id_b = ((const ModData *)b)->Id;
    if (id_a > id_b) return 1;
    if (id_a < id_b) return -1;
    return 0;
}

static void write_mods_sorted(const char *filename, ModData *mods, int count, int by_name) {
    ModData *sorted = (ModData *)malloc(sizeof(ModData) * count);
    if (!sorted) return;
    memcpy(sorted, mods, sizeof(ModData) * count);
    if (by_name) qsort(sorted, count, sizeof(ModData), compare_mods_by_name);
    else qsort(sorted, count, sizeof(ModData), compare_mods_by_updated);
    write_mods_json(filename, sorted, count, 0);
    free(sorted);
}

static int fetch_category(CURL *curl, int cat_id, ModData **out_mods) {
    char url[MAX_URL];
    int w_len = snprintf(url, sizeof(url), "%s?_nPage=1&_nPerpage=50&_aFilters[Generic_Category]=%d", API_V10_INDEX, cat_id);
    if (w_len < 0 || (size_t)w_len >= sizeof(url)) return 0;

    FetchResult result = curl_get_retry(curl, url, MAX_FETCH_ATTEMPTS);
    if (!result.success) {
        free_fetch_result(&result);
        printf("Category %d failed after retries, skipping\n", cat_id);
        svcSleepThread(700000000ULL);
        return 0;
    }

    json_object *root = json_tokener_parse(result.data);
    free_fetch_result(&result);
    if (!root) return 0;

    int count = 0;
    json_object *records;
    if (!json_object_object_get_ex(root, "_aRecords", &records) || json_object_get_type(records) != json_type_array) {
        json_object_put(root);
        return 0;
    }

    int len = json_object_array_length(records);
    if (len <= 0) {
        json_object_put(root);
        *out_mods = NULL;
        return 0;
    }

    *out_mods = (ModData *)malloc(sizeof(ModData) * len);
    if (!*out_mods) { json_object_put(root); return 0; }

    for (int i = 0; i < len; i++) {
        json_object *record = json_object_array_get_idx(records, i);
        if (!record || json_object_get_type(record) != json_type_object) continue;

        int id = get_json_int(record, "_idRow");
        if (id == 0) continue;

        ModData mod;
        memset(&mod, 0, sizeof(mod));
        mod.Id = id;

        const char *sName = get_json_string(record, "_sName");
        if (sName) snprintf(mod.Name, sizeof(mod.Name), "%s", sName);

        snprintf(mod.Author, sizeof(mod.Author), "Unknown");

        json_object *submitter;
        if (json_object_object_get_ex(record, "_aSubmitter", &submitter) && json_object_get_type(submitter) == json_type_object) {
            const char *subName = get_json_string(submitter, "_sName");
            if (subName) snprintf(mod.Author, sizeof(mod.Author), "%s", subName);
        }

        json_object *preview;
        if (json_object_object_get_ex(record, "_aPreviewMedia", &preview) && json_object_get_type(preview) == json_type_object) {
            json_object *images;
            if (json_object_object_get_ex(preview, "_aImages", &images) && json_object_get_type(images) == json_type_array) {
                int img_count = json_object_array_length(images);
                for (int j = 0; j < img_count && j < 1; j++) {
                    json_object *img = json_object_array_get_idx(images, j);
                    if (!img || json_object_get_type(img) != json_type_object) continue;

                    const char *base_url = get_json_string(img, "_sBaseUrl");
                    const char *file220 = get_json_string(img, "_sFile220");
                    const char *file = get_json_string(img, "_sFile");

                    if (base_url && file220 && base_url[0] && file220[0]) {
                        snprintf(mod.ThumbnailUrl, sizeof(mod.ThumbnailUrl), "%s/%s", base_url, file220);
                    } else if (base_url && file && base_url[0] && file[0]) {
                        snprintf(mod.ThumbnailUrl, sizeof(mod.ThumbnailUrl), "%s/%s", base_url, file);
                    }
                }
            }
        }
        (*out_mods)[count++] = mod;
    }

    json_object_put(root);
    return count;
}

static int deduplicate_mods(ModData *mods, int count) {
    if (count <= 1) return count;

    qsort(mods, count, sizeof(ModData), compare_mods_by_id);

    int unique_count = 0;
    for (int i = 0; i < count; i++) {
        if (i == 0 || mods[i].Id != mods[i-1].Id) {
            mods[unique_count++] = mods[i];
        }
    }
    return unique_count;
}

static ModData* search_mods_by_id(ModData *mods, int count, int id) {
    int low = 0;
    int high = count - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (mods[mid].Id == id) return &mods[mid];
        if (mods[mid].Id < id) low = mid + 1;
        else high = mid - 1;
    }
    return NULL;
}

static void parse_latest_file(json_object *item, ModData *mod) {
    if (json_object_get_type(item) == json_type_array) {
        int arr_len = json_object_array_length(item);
        if (arr_len > 0) {
            item = json_object_array_get_idx(item, 0);
        } else {
            return;
        }
    }

    if (json_object_get_type(item) != json_type_object) return;

    int64_t max_ts = 0;

    json_object *files_obj;
    if (json_object_object_get_ex(item, "aFiles", &files_obj) && json_object_get_type(files_obj) == json_type_array) {
        int files_len = json_object_array_length(files_obj);
        for (int i = 0; i < files_len; i++) {
            json_object *file_obj = json_object_array_get_idx(files_obj, i);
            if (file_obj && json_object_get_type(file_obj) == json_type_object) {
                const char *sfile = get_json_string(file_obj, "_sFile");
                int64_t ts = get_json_int64(file_obj, "_tsDateAdded");
                int fid = get_json_int(file_obj, "_idRow");
                if (sfile && sfile[0] && ts > max_ts) {
                    max_ts = ts;
                    snprintf(mod->LatestFileUrl, sizeof(mod->LatestFileUrl), "https://gamebanana.com/dl/%d", fid);
                    snprintf(mod->LatestFileName, sizeof(mod->LatestFileName), "%s", sfile);
                    mod->LatestFileDate = max_ts;
                }
            }
        }
    } else {
        json_object_object_foreach(item, key, file_obj) {
            (void)key;
            if (file_obj && json_object_get_type(file_obj) == json_type_object) {
                const char *sfile = get_json_string(file_obj, "_sFile");
                int64_t ts = get_json_int64(file_obj, "_tsDateAdded");
                int fid = get_json_int(file_obj, "_idRow");
                if (sfile && sfile[0] && ts > max_ts) {
                    max_ts = ts;
                    snprintf(mod->LatestFileUrl, sizeof(mod->LatestFileUrl), "https://gamebanana.com/dl/%d", fid);
                    snprintf(mod->LatestFileName, sizeof(mod->LatestFileName), "%s", sfile);
                    mod->LatestFileDate = max_ts;
                }
            }
        }
    }
}

static void fetch_core_data(CURL *curl, ModData *mods, int count) {
    printf("Fetching core data for %d items...\n", count);

    const int BATCH_SIZE = 20;

    for (int i = 0; i < count; i += BATCH_SIZE) {
        char url[MAX_URL];
        int url_len = snprintf(url, sizeof(url), "%s?", API_CORE_DATA);
        if (url_len < 0 || (size_t)url_len >= sizeof(url)) continue;

        int current_batch = 0;
        ModData* batch_ptrs[BATCH_SIZE];
        
        for (int j = 0; j < BATCH_SIZE && (i + j) < count; j++) {
            if (mods[i+j].LatestFileUrl[0]) continue;
            
            int added = snprintf(url + url_len, sizeof(url) - url_len, 
                "itemtype[]=Mod&itemid[]=%d&fields[]=Files().aFiles()&", mods[i+j].Id);
            
            if (added < 0 || (size_t)added >= sizeof(url) - url_len) {
                break;
            }
            url_len += added;
            
            batch_ptrs[current_batch] = &mods[i+j];
            current_batch++;
        }
        
        if (current_batch == 0) continue;
        
        if (url_len > 0 && url[url_len-1] == '&') {
            url[url_len-1] = '\0';
        }
        
        printf("[%d/%d] Fetching batch of %d mods...\n",
               (i + current_batch > count ? count : i + current_batch),
               count, current_batch);
               
        FetchResult result = curl_get_retry(curl, url, MAX_FETCH_ATTEMPTS);
        if (!result.success) {
            free_fetch_result(&result);
            printf("Batch fetch failed after retries.\n");
            svcSleepThread(700000000ULL);
            return;
        }
        
        json_object *root = json_tokener_parse(result.data);
        free_fetch_result(&result);
        if (!root || json_object_get_type(root) != json_type_array) {
            if (root) json_object_put(root);
            return;
        }
        
        int arr_len = json_object_array_length(root);
        for (int k = 0; k < arr_len && k < current_batch; k++) {
            json_object *item = json_object_array_get_idx(root, k);
            if (!item) continue;
            
            ModData *mod = search_mods_by_id(mods, count, batch_ptrs[k]->Id);
            if (mod) {
                parse_latest_file(item, mod);
            }
        }
        json_object_put(root);
    }
}

extern int gui(); // gui.cpp

int main(int argc, char **argv) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("\x1b[1;1HGamebananaFetcher 3DS Port\n");
    if (log_to_file) {
        // this doesn't need a str substitution but i'm leaving it here incase
        printf("Logging to %s\n", LOG_FILE);
        freopen(LOG_FILE, "w", stdout);
    }

    if (!romfs_mounted) {
        printf("romfsInit failed - no CA bundle, cannot verify TLS!\n");
        svcSleepThread(2000000000ULL);
        goto exit_no_soc;
    }

    if (gui()) goto exit_no_gui;

    // below here needs to be on another thread

    socBuffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!socBuffer) {
        printf("Failed to allocate SOC buffer!\n");
        goto exit_no_soc;
    }
    if (R_FAILED(socInit(socBuffer, SOC_BUFFERSIZE))) {
        printf("socInit failed!\n");
        goto exit_soc_fail;
    }

    if (R_FAILED(sslcInit(0))) {
        printf("sslcInit failed!\n");
        goto exit_sslc_fail;
    }

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        printf("curl_global_init failed!\n");
        goto exit_curl_fail;
    }

    curl = curl_easy_init();
    if (!curl) {
        printf("curl_easy_init failed!\n");
        goto exit_curl_handle_fail;
    }

    rmrf(CACHE_DIR);
    init_paths();

    printf("Fetching %d categories...\n", NUM_CATEGORIES);

    ModData *all_mods = NULL;
    int total_count = 0;

    for (int i = 0; i < NUM_CATEGORIES; i++) {
        printf("Fetching category %d/%d...\n", i + 1, NUM_CATEGORIES);
        ModData *cat_mods = NULL;
        int cat_count = fetch_category(curl, CATEGORIES[i], &cat_mods);
        if (cat_count > 0 && cat_mods) {
            ModData *tmp = (ModData *)realloc(all_mods, sizeof(ModData) * (total_count + cat_count));
            if (!tmp) {
                free(cat_mods);
                break;
            }
            all_mods = tmp;
            memcpy(all_mods + total_count, cat_mods, sizeof(ModData) * cat_count);
            total_count += cat_count;
        }
        if (cat_mods) free(cat_mods);
    }

    if (total_count > 0 && all_mods) {
        total_count = deduplicate_mods(all_mods, total_count);

        fetch_core_data(curl, all_mods, total_count);
        
        if (!write_mods_json(MOD_LIST_FILE, all_mods, total_count)) {
            printf("Error saving mod list!\n");
        }

        qsort(all_mods, total_count, sizeof(ModData), compare_mods_by_name);
        write_mods_json(BY_NAME_FILE, all_mods, total_count);

        qsort(all_mods, total_count, sizeof(ModData), compare_mods_by_updated);
        write_mods_json(BY_UPDATED_FILE, all_mods, total_count);
        
        printf("Done! Files saved.\n");
        free(all_mods);
    } else {
        printf("Failed to fetch any mods!\n");
    }

    svcSleepThread(1500000000ULL);

    printf("Press START to exit.\n");
    // above here needs to be on another thread

    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        // TODO move this to gui.cpp
    }

    curl_easy_cleanup(curl);

exit_curl_handle_fail:
    curl_global_cleanup();

exit_curl_fail:
    sslcExit();
exit_sslc_fail:
    socExit();
exit_soc_fail:
    free(socBuffer);
exit_no_soc:
exit_no_gui:
    if (romfs_mounted) romfsExit();

    gfxExit();
    return (romfs_mounted && socBuffer && curl) ? 0 : 1;
}