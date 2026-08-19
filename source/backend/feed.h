#pragma once

#include "backend/store.h"

#include <curl/curl.h>
#include <json-c/json.h>

#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

namespace mm {
namespace feed {

using store::ModData;

struct PageResult {
  std::vector<ModData> mods;
  int record_count = -1;
  bool ok = false;
};

struct PageJob {
  int category_id = 0;
  int page = 1;
  bool sequential = false;
};

extern std::atomic<std::size_t> core_batch_cap;
extern std::atomic<bool> quit_requested;

void parse_index_records(json_object *records, std::vector<ModData> &out);

[[nodiscard]] bool fetch_index_page(CURL *curl, int category_id, int page, PageResult &out);

void collect_pages(const std::vector<PageJob> &jobs, std::vector<PageResult> &results);

void deduplicate(std::vector<ModData> &mods);

void parse_latest_file(json_object *raw_item, ModData &mod);

void lower_core_cap(std::size_t limit) noexcept;

void fetch_core_range(CURL *curl, std::span<ModData> mods);

void fetch_core_data(std::vector<ModData> &mods);

void drain_pages(std::vector<PageResult> &results, std::vector<ModData> &out);

[[nodiscard]] std::vector<PageJob> plan_remaining_pages(const std::vector<PageResult> &first_pass);

void run_pipeline();

void thread_main(void *);

}  // namespace feed
}  // namespace mm
