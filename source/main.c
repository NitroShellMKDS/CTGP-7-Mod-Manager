#include <3ds.h>
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

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000
static u32 *socBuffer = NULL;

static const char *USER_AGENT = "CTGP-7-Mod-Manager/3.0";
static const char *API_V10_INDEX = "https://gamebanana.com/apiv10/Mod/Index";
static const char *API_CORE_DATA = "https://api.gamebanana.com/Core/Item/Data";

static const int CATEGORIES[] = {35931, 10605, 35932, 35943, 35933, 35935, 35937, 35938, 35939, 35941, 35942, 35944, 35946, 35947, 35945, 35940, 35934, 35936};
static const int NUM_CATEGORIES = sizeof(CATEGORIES) / sizeof(CATEGORIES[0]);

static const bool log_to_file = false;
#define log_loc "sdmc:/3ds/CTGP-7-Mod-Manager/output.log"

#define MAX_PATH 512
#define MAX_URL 2048

static char BASE_DIR[MAX_PATH];
static char APP_DIR[MAX_PATH];
static char CTGP7_DIR[MAX_PATH];
static char STATE_FILE[MAX_PATH];
static char LOG_FILE[MAX_PATH];
static char LISTS_DIR[MAX_PATH];
static char MOD_LIST_FILE[MAX_PATH];
static char BY_NAME_FILE[MAX_PATH];
static char BY_UPDATED_FILE[MAX_PATH];
static char THUMBNAIL_CACHE_DIR[MAX_PATH];

typedef struct {
    int Id;
    char Name[256];
    char Author[128];
    char ThumbnailUrl[512];
    char LatestFileUrl[512];
    long long LatestFileDate;
    char LatestFileName[256];
} ModData;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
    long response_code;
    bool success;
} FetchResult;

static void init_paths(void) {
    snprintf(BASE_DIR, sizeof(BASE_DIR), "sdmc:/");
    snprintf(APP_DIR, sizeof(APP_DIR), "sdmc:/3ds/CTGP-7-Mod-Manager");
    snprintf(CTGP7_DIR, sizeof(CTGP7_DIR), "sdmc:/CTGP-7/MyStuff/Characters");
    snprintf(STATE_FILE, sizeof(STATE_FILE), "sdmc:/3ds/CTGP-7-Mod-Manager/installed_mods.json");
    snprintf(LOG_FILE, sizeof(LOG_FILE), "sdmc:/3ds/CTGP-7-Mod-Manager/log.txt");
    snprintf(LISTS_DIR, sizeof(LISTS_DIR), "sdmc:/3ds/CTGP-7-Mod-Manager/cache/lists");
    snprintf(MOD_LIST_FILE, sizeof(MOD_LIST_FILE), "sdmc:/3ds/CTGP-7-Mod-Manager/cache/lists/modlist.json");
    snprintf(BY_NAME_FILE, sizeof(BY_NAME_FILE), "sdmc:/3ds/CTGP-7-Mod-Manager/cache/lists/byname.json");
    snprintf(BY_UPDATED_FILE, sizeof(BY_UPDATED_FILE), "sdmc:/3ds/CTGP-7-Mod-Manager/cache/lists/byupdated.json");
    snprintf(THUMBNAIL_CACHE_DIR, sizeof(THUMBNAIL_CACHE_DIR), "sdmc:/3ds/CTGP-7-Mod-Manager/cache/images");
}

static void mkdir_p(const char *path) {
    char buf[MAX_PATH];
    snprintf(buf, sizeof(buf), "%s", path);

    size_t prefix_len = strlen("sdmc:/");
    char *start = buf;
    if (strncmp(buf, "sdmc:/", prefix_len) == 0) {
        start = buf + prefix_len;
    }

    for (char *p = start; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0777); 
            *p = '/';
        }
    }
    mkdir(buf, 0777);
}

static void ensure_directories(void) {
    mkdir_p(APP_DIR);
    mkdir_p(LISTS_DIR);
    mkdir_p(THUMBNAIL_CACHE_DIR);
    mkdir_p(CTGP7_DIR);
}

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    FetchResult *result = (FetchResult *)userp;
    if (result->size + total + 1 > result->capacity) {
        size_t new_cap = result->capacity * 2;
        if (new_cap < result->size + total + 1) new_cap = result->size + total + 1;
        char *new_data = (char *)realloc(result->data, new_cap);
        if (!new_data) return 0;
        result->data = new_data;
        result->capacity = new_cap;
    }
    memcpy(result->data + result->size, contents, total);
    result->size += total;
    result->data[result->size] = '\0';
    return total;
}

static FetchResult curl_get(const char *url) {
    FetchResult result;
    memset(&result, 0, sizeof(result));
    result.capacity = 4096;
    result.data = (char *)malloc(result.capacity);
    if (!result.data) return result;

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(result.data);
        memset(&result, 0, sizeof(result));
        return result;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.response_code);
        if (result.response_code >= 200 && result.response_code < 300) {
            result.success = true;
        }
    }

    curl_easy_cleanup(curl);
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
    return "";
}

static int get_json_int(json_object *obj, const char *key) {
    json_object *val;
    if (json_object_object_get_ex(obj, key, &val) && json_object_get_type(val) == json_type_int) {
        return json_object_get_int(val);
    }
    return 0;
}

static long long get_json_llong(json_object *obj, const char *key) {
    json_object *val;
    if (json_object_object_get_ex(obj, key, &val) && json_object_get_type(val) == json_type_int) {
        return (long long)json_object_get_int64(val);
    }
    return 0;
}

static void escape_json_string(const char *src, char *dst, int dst_size) {
    int di = 0;
    for (int i = 0; src[i] && di < dst_size - 2; i++) {
        char c = src[i];
        if (c == '"') { if (di + 2 < dst_size) { dst[di++] = '\\'; dst[di++] = '"'; } }
        else if (c == '\\') { if (di + 2 < dst_size) { dst[di++] = '\\'; dst[di++] = '\\'; } }
        else if (c == '\n') { if (di + 2 < dst_size) { dst[di++] = '\\'; dst[di++] = 'n'; } }
        else if (c == '\r') { if (di + 2 < dst_size) { dst[di++] = '\\'; dst[di++] = 'r'; } }
        else if (c == '\t') { if (di + 2 < dst_size) { dst[di++] = '\\'; dst[di++] = 't'; } }
        else dst[di++] = c;
    }
    dst[di] = '\0';
}

static void write_mods_json(const char *filename, ModData *mods, int count, int sort_by_name) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;

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
        fprintf(f, "\"LatestFileDate\":%lld,", mods[i].LatestFileDate);
        fprintf(f, "\"LatestFileName\":\"%s\"}", fname_esc);
        if (i < count - 1) fprintf(f, ",");
        fprintf(f, "\n");
    }
    fprintf(f, "]\n");
    fclose(f);
}

static int compare_mods_by_name(const void *a, const void *b) {
    const ModData *ma = (const ModData *)a;
    const ModData *mb = (const ModData *)b;
    return strcmp(ma->Name, mb->Name);
}

static int compare_mods_by_updated(const void *a, const void *b) {
    const ModData *ma = (const ModData *)a;
    const ModData *mb = (const ModData *)b;
    if (mb->LatestFileDate > ma->LatestFileDate) return 1;
    if (mb->LatestFileDate < ma->LatestFileDate) return -1;
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

static int fetch_category(int cat_id, ModData **out_mods) {
    char url[MAX_URL];
    snprintf(url, sizeof(url), "%s?_nPage=1&_nPerpage=50&_aFilters[Generic_Category]=%d", API_V10_INDEX, cat_id);

    FetchResult result = curl_get(url);
    if (!result.success) {
        free_fetch_result(&result);
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
    *out_mods = (ModData *)malloc(sizeof(ModData) * len);
    if (!*out_mods) { json_object_put(root); return 0; }

    for (int i = 0; i < len; i++) {
        json_object *record = json_object_array_get_idx(records, i);
        if (!record || json_object_get_type(record) != json_type_object) continue;

        int id = get_json_int(record, "_idRow");
        if (id == 0) {
            json_object *id_obj;
            if (json_object_object_get_ex(record, "_idRow", &id_obj) && json_object_get_type(id_obj) == json_type_int) {
                id = json_object_get_int(id_obj);
            }
        }
        if (id == 0) continue;

        ModData mod;
        memset(&mod, 0, sizeof(mod));
        mod.Id = id;
        strncpy(mod.Name, get_json_string(record, "_sName"), sizeof(mod.Name) - 1);
        strncpy(mod.Author, "Unknown", sizeof(mod.Author) - 1);

        json_object *submitter;
        if (json_object_object_get_ex(record, "_aSubmitter", &submitter) && json_object_get_type(submitter) == json_type_object) {
            strncpy(mod.Author, get_json_string(submitter, "_sName"), sizeof(mod.Author) - 1);
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
                    if (base_url[0] && file220[0]) {
                        snprintf(mod.ThumbnailUrl, sizeof(mod.ThumbnailUrl), "%s/%s", base_url, file220);
                    } else if (base_url[0] && file[0]) {
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

static int compare_mods_by_id(const void *a, const void *b) {
    return ((const ModData *)a)->Id - ((const ModData *)b)->Id;
}

static void parse_latest_file(json_object *item, ModData *mod) {
    json_object *files;
    if (!json_object_object_get_ex(item, "Files().aFiles()", &files)) return;

    if (json_object_get_type(files) == json_type_array) {
        int arr_len = json_object_array_length(files);
        if (arr_len > 0) {
            json_object *first = json_object_array_get_idx(files, 0);
            if (first && json_object_get_type(first) == json_type_object) {
                files = first;
            } else {
                return;
            }
        }
    }

    if (json_object_get_type(files) != json_type_object) return;

    long long max_ts = 0;

    json_object_object_foreach(files, key, file_obj) {
        (void)key;
        if (file_obj && json_object_get_type(file_obj) == json_type_object) {
            const char *sfile = get_json_string(file_obj, "_sFile");
            long long ts = get_json_llong(file_obj, "_tsDateAdded");
            int fid = get_json_int(file_obj, "_idRow");
            if (sfile[0] && ts > max_ts) {
                max_ts = ts;
                snprintf(mod->LatestFileUrl, sizeof(mod->LatestFileUrl), "https://gamebanana.com/dl/%d", fid);
                strncpy(mod->LatestFileName, sfile, sizeof(mod->LatestFileName) - 1);
                mod->LatestFileDate = max_ts;
            }
        }
    }
}

static void fetch_core_data(ModData *mods, int count) {
    int unique_count = 0;
    ModData *unique = (ModData *)malloc(sizeof(ModData) * count);
    memset(unique, 0, sizeof(ModData) * count);

    for (int i = 0; i < count; i++) {
        int found = 0;
        for (int j = 0; j < unique_count; j++) {
            if (unique[j].Id == mods[i].Id) { found = 1; break; }
        }
        if (!found) unique[unique_count++] = mods[i];
    }

    printf("Fetching core data for %d items (Batched)...\n", unique_count);

    const int BATCH_SIZE = 20;

    for (int i = 0; i < unique_count; i += BATCH_SIZE) {
        char url[MAX_URL];
        int url_len = snprintf(url, sizeof(url), "%s?", API_CORE_DATA);
        
        int current_batch = 0;
        ModData* batch_ptrs[BATCH_SIZE];
        
        for (int j = 0; j < BATCH_SIZE && (i + j) < unique_count; j++) {
            if (unique[i+j].LatestFileUrl[0]) continue;
            
            int added = snprintf(url + url_len, sizeof(url) - url_len, 
                "itemtype[]=Mod&itemid[]=%d&fields[]=Files().aFiles()&", unique[i+j].Id);
            
            if (url_len + added >= sizeof(url)) break;
            url_len += added;
            
            batch_ptrs[current_batch] = &unique[i+j];
            current_batch++;
        }
        
        if (current_batch == 0) continue;
        
        if (url_len > 0 && url[url_len-1] == '&') {
            url[url_len-1] = '\0';
        }
        
        printf("\x1b[2K\r[%d/%d] Fetching batch of %d mods...", 
               (i + current_batch > unique_count ? unique_count : i + current_batch), 
               unique_count, current_batch);
               
        FetchResult result = curl_get(url);
        if (!result.success) {
            free_fetch_result(&result);
            continue; 
        }
        
        json_object *root = json_tokener_parse(result.data);
        free_fetch_result(&result);
        if (!root || json_object_get_type(root) != json_type_array) {
            if (root) json_object_put(root);
            continue;
        }
        
        int arr_len = json_object_array_length(root);
        for (int k = 0; k < arr_len && k < current_batch; k++) {
            json_object *item = json_object_array_get_idx(root, k);
            if (!item) continue;
            
            for (int m = 0; m < count; m++) {
                if (mods[m].Id == batch_ptrs[k]->Id) {
                    parse_latest_file(item, &mods[m]);
                }
            }
        }
        json_object_put(root);
    }
    
    printf("\n");
    free(unique);
}

int main(int argc, char **argv) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("\x1b[1;1HGamebananaFetcher 3DS Port\n");
    if (log_to_file) {
        printf("Logging to " log_loc "\n");
        freopen(log_loc, "w", stdout);
    }

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

    init_paths();
    ensure_directories();
    
    printf("Fetching %d categories...\n", NUM_CATEGORIES);

    ModData *all_mods = NULL;
    int total_count = 0;

    for (int i = 0; i < NUM_CATEGORIES; i++) {
        printf("Fetching category %d/%d (ID %d)...\n", i + 1, NUM_CATEGORIES, CATEGORIES[i]);
        ModData *cat_mods = NULL;
        int cat_count = fetch_category(CATEGORIES[i], &cat_mods);
        if (cat_count > 0 && cat_mods) {
            ModData *tmp = (ModData *)realloc(all_mods, sizeof(ModData) * (total_count + cat_count));
            if (!tmp) {
                free(cat_mods);
                break;
            }
            all_mods = tmp;
            memcpy(all_mods + total_count, cat_mods, sizeof(ModData) * cat_count);
            total_count += cat_count;
            free(cat_mods);
        }
    }

    if (total_count > 0 && all_mods) {
        printf("Total mods fetched: %d\n", total_count);
        qsort(all_mods, total_count, sizeof(ModData), compare_mods_by_id);
        
        fetch_core_data(all_mods, total_count);
        
        write_mods_json(MOD_LIST_FILE, all_mods, total_count, 0);
        write_mods_sorted(BY_NAME_FILE, all_mods, total_count, 1);
        write_mods_sorted(BY_UPDATED_FILE, all_mods, total_count, 0);
        printf("Done! Files saved to %s\n", LISTS_DIR);
        free(all_mods);
    } else {
        printf("Failed to fetch any mods!\n");
    }

    printf("\nPress START to exit.\n");

    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
    }

    curl_global_cleanup();

exit_curl_fail:
    sslcExit();
exit_sslc_fail:
    socExit();
exit_soc_fail:
    free(socBuffer);
exit_no_soc:
    
    gfxExit();
    fflush(stdout);
    return 0;
}