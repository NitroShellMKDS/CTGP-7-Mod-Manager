#include "backend/feed.h"

#include "backend/json.h"
#include "backend/net.h"
#include "backend/sd_card.h"
#include "core/config.h"
#include "core/format.h"
#include "core/progress.h"
#include "core/status.h"
#include "core/system.h"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <string>
#include <utility>

namespace mm {
namespace feed {

std::atomic<std::size_t> core_batch_cap{cfg::CORE_BATCH_SIZE};
std::atomic<bool> quit_requested{false};

void parse_index_records(json_object *records, std::vector<ModData> &out) {
  for (json_object *record : js::ArrayView{records}) {
    if (!js::is(record, json_type_object)) {
      continue;
    }
    const char *nsfw = js::string_field(record, "_sInitialVisibility");
    if (!nsfw || std::string_view{nsfw} != "show") continue;
    const int id = js::integer_field<int>(record, "_idRow");
    if (id == 0) {
      continue;
    }
    ModData mod;
    mod.id = id;
    mod.name = js::string_or_empty(record, "_sName");
    mod.author = "Unknown";
    if (json_object *submitter = js::field(record, "_aSubmitter", json_type_object)) {
      const char *name = js::string_field(submitter, "_sName");
      if (name != nullptr && name[0] != '\0') {
        mod.author = name;
      }
    }
    if (json_object *preview = js::field(record, "_aPreviewMedia", json_type_object)) {
      const js::ArrayView images = js::array_field(preview, "_aImages");
      if (!images.empty()) {
        json_object *image = *images.begin();
        if (js::is(image, json_type_object)) {
          const char *base = js::string_field(image, "_sBaseUrl");
          const char *file220 = js::string_field(image, "_sFile220");
          const char *file = js::string_field(image, "_sFile");
          const char *chosen = nullptr;
          if (file220 != nullptr && file220[0] != '\0') {
            chosen = file220;
          } else if (file != nullptr && file[0] != '\0') {
            chosen = file;
          }
          if (base != nullptr && base[0] != '\0' && chosen != nullptr) {
            mod.thumbnail_url = fmt::format("{}/{}", base, chosen);
          }
        }
      }
    }
    out.push_back(std::move(mod));
  }
}

bool fetch_index_page(CURL *curl, int category_id, int page, PageResult &out) {
  const std::string url = fmt::format(
      "{}?_nPage={}&_nPerpage={}&_aFilters[Generic_Category]={}",
      cfg::API_V10_INDEX, page, cfg::INDEX_PER_PAGE, category_id);
  const net::Response response = net::get_with_retry(curl, url, cfg::MAX_FETCH_ATTEMPTS);
  if (!response.ok) {
    return false;
  }
  const sys::JsonRef root{json_tokener_parse(response.data.c_str())};
  if (!root) {
    return false;
  }
  if (json_object *meta = js::field(root.get(), "_aMetadata", json_type_object)) {
    out.record_count = js::integer_field<int>(meta, "_nRecordCount");
  }
  parse_index_records(js::field(root.get(), "_aRecords", json_type_array), out.mods);
  out.ok = true;
  return true;
}

void collect_pages(const std::vector<PageJob> &jobs, std::vector<PageResult> &results) {
  results.clear();
  results.resize(jobs.size());
  if (jobs.empty()) {
    return;
  }
  auto run_page_job = [&jobs, &results](CURL *curl, std::size_t index) {
    const PageJob &job = jobs[index];
    PageResult &out = results[index];
    if (quit_requested.load(std::memory_order_acquire)) {
      return;
    }
    if (!job.sequential) {
      (void)fetch_index_page(curl, job.category_id, job.page, out);
    } else {
      for (int page = job.page; page < job.page + cfg::MAX_PAGES_PER_CAT; ++page) {
        if (quit_requested.load(std::memory_order_acquire)) {
          break;
        }
        const std::size_t before = out.mods.size();
        if (!fetch_index_page(curl, job.category_id, page, out)) {
          break;
        }
        if (out.mods.size() - before < static_cast<std::size_t>(cfg::INDEX_PER_PAGE)) {
          break;
        }
      }
    }
    progress::step();
  };
  net::run_jobs(jobs.size(), run_page_job);
}

void deduplicate(std::vector<ModData> &mods) {
  if (mods.size() <= 1) {
    return;
  }
  std::ranges::sort(mods, {}, &ModData::id);
  const auto duplicates = std::ranges::unique(mods, {}, &ModData::id);
  mods.erase(duplicates.begin(), duplicates.end());
}

void parse_latest_file(json_object *raw_item, ModData &mod) {
  json_object *item = raw_item;
  if (js::is(raw_item, json_type_array)) {
    const js::ArrayView wrapper{raw_item};
    if (wrapper.empty()) {
      return;
    }
    item = *wrapper.begin();
  }
  if (!js::is(item, json_type_object)) {
    return;
  }
  int64_t newest = 0;
  const auto consider = [&mod, &newest](json_object *file) {
    if (!js::is(file, json_type_object)) {
      return;
    }
    const int file_id = js::integer_field<int>(file, "_idRow");
    const int64_t added = js::integer_field<int64_t>(file, "_tsDateAdded");
    const char *const name = js::string_field(file, "_sFile");
    if (file_id > 0 && name != nullptr && name[0] != '\0' && added > newest) {
      newest = added;
      mod.latest_file_url = fmt::format("{}{}", cfg::DOWNLOAD_BASE, file_id);
      mod.latest_file_name = name;
      mod.latest_file_date = added;
    }
  };
  const js::ArrayView files = js::array_field(item, "aFiles");
  if (!files.empty()) {
    for (json_object *file : files) {
      consider(file);
    }
    return;
  }
  json_object_object_foreach(item, key, value) {
    (void)key;
    consider(value);
  }
}

void lower_core_cap(std::size_t limit) noexcept {
  const std::size_t target = std::max<std::size_t>(limit, 1);
  std::size_t current = core_batch_cap.load(std::memory_order_acquire);
  while (target < current &&
         !core_batch_cap.compare_exchange_weak(current, target, std::memory_order_acquire)) {
  }
}

void fetch_core_range(CURL *curl, std::span<ModData> mods) {
  if (mods.empty() || quit_requested.load(std::memory_order_acquire)) {
    return;
  }
  const auto split = [curl](std::span<ModData> range) {
    const std::size_t half = range.size() / 2;
    fetch_core_range(curl, range.first(half));
    fetch_core_range(curl, range.subspan(half));
  };
  if (mods.size() > 1 && mods.size() > core_batch_cap.load(std::memory_order_acquire)) {
    split(mods);
    return;
  }
  std::vector<ModData *> targets;
  targets.reserve(mods.size());
  constexpr std::size_t MAX_URL_LENGTH = 4096;
  std::string url{cfg::API_CORE_DATA};
  url += '?';
  for (ModData &mod : mods) {
    if (!mod.latest_file_url.empty()) {
      continue;
    }
    const std::string part =
        fmt::format("itemtype[]=Mod&itemid[]={}&fields[]=Files().aFiles()&", mod.id);
    if (url.size() + part.size() > MAX_URL_LENGTH) {
      break;
    }
    url += part;
    targets.push_back(&mod);
  }
  if (targets.empty()) {
    return;
  }
  if (url.back() == '&') {
    url.pop_back();
  }
  bool resolved = false;
  bool too_big = false;
  const net::Response response = net::get_with_retry(curl, url, cfg::MAX_FETCH_ATTEMPTS);
  if (response.ok) {
    const sys::JsonRef root{json_tokener_parse(response.data.c_str())};
    if (root) {
      if (targets.size() == 1) {
        parse_latest_file(root.get(), *targets[0]);
        resolved = true;
      } else {
        const js::ArrayView items{root.get()};
        if (items.size() == targets.size()) {
          std::size_t index = 0;
          for (json_object *item : items) {
            if (item != nullptr) {
              parse_latest_file(item, *targets[index]);
            }
            ++index;
          }
          resolved = true;
        }
      }
    }
    too_big = !resolved;
  } else if (net::request_too_large(response.status_code)) {
    too_big = true;
  }
  if (resolved || mods.size() == 1) {
    return;
  }
  if (too_big) {
    lower_core_cap(mods.size() / 2);
  }
  split(mods);
}

void fetch_core_data(std::vector<ModData> &mods) {
  if (mods.empty()) {
    return;
  }
  const std::size_t total = mods.size();
  const std::size_t batches = (total + cfg::CORE_BATCH_SIZE - 1) / cfg::CORE_BATCH_SIZE;
  auto run_batch = [&mods, total](CURL *curl, std::size_t index) {
    if (quit_requested.load(std::memory_order_acquire)) {
      return;
    }
    const std::size_t first = index * cfg::CORE_BATCH_SIZE;
    const std::size_t count = std::min(cfg::CORE_BATCH_SIZE, total - first);
    fetch_core_range(curl, std::span{mods}.subspan(first, count));
    progress::step();
  };
  progress::begin("Resolving downloads ", batches);
  net::run_jobs(batches, run_batch);
}

void drain_pages(std::vector<PageResult> &results, std::vector<ModData> &out) {
  std::size_t incoming = 0;
  for (const PageResult &result : results) {
    incoming += result.mods.size();
  }
  out.reserve(out.size() + incoming);
  for (PageResult &result : results) {
    out.insert(out.end(),
               std::make_move_iterator(result.mods.begin()),
               std::make_move_iterator(result.mods.end()));
    std::vector<ModData>{}.swap(result.mods);
  }
}

std::vector<PageJob> plan_remaining_pages(const std::vector<PageResult> &first_pass) {
  std::vector<PageJob> jobs;
  const std::size_t count = std::min(first_pass.size(), cfg::CATEGORIES.size());
  for (std::size_t i = 0; i < count; ++i) {
    const PageResult &result = first_pass[i];
    if (!result.ok ||
        result.mods.size() < static_cast<std::size_t>(cfg::INDEX_PER_PAGE)) {
      continue;
    }
    PageJob job;
    job.category_id = cfg::CATEGORIES[i];
    if (result.record_count > cfg::INDEX_PER_PAGE) {
      int record_count = std::max(0, result.record_count);
      int pages = (record_count + cfg::INDEX_PER_PAGE - 1) / cfg::INDEX_PER_PAGE;
      pages = std::clamp(pages, 0, cfg::MAX_PAGES_PER_CAT);
      for (int page = 2; page < pages; ++page) {
        job.page = page;
        job.sequential = false;
        jobs.push_back(job);
      }
      job.page = pages;
    } else {
      job.page = 2;
    }
    job.sequential = true;
    jobs.push_back(job);
  }
  return jobs;
}

void run_pipeline() {
  if (quit_requested.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<ModData> all_mods;
  std::vector<PageJob> first_jobs(cfg::CATEGORIES.size());
  for (std::size_t i = 0; i < cfg::CATEGORIES.size(); ++i) {
    first_jobs[i].category_id = cfg::CATEGORIES[i];
    first_jobs[i].page = 1;
  }
  std::vector<PageResult> first_results;
  progress::begin("Scanning categories ", first_jobs.size());
  collect_pages(first_jobs, first_results);
  if (quit_requested.load(std::memory_order_acquire)) {
    status::print("Feed cancelled.");
    return;
  }
  const std::vector<PageJob> more_jobs = plan_remaining_pages(first_results);
  drain_pages(first_results, all_mods);
  if (!more_jobs.empty()) {
    std::vector<PageResult> more_results;
    progress::begin("Fetching mod pages ", more_jobs.size());
    collect_pages(more_jobs, more_results);
    if (quit_requested.load(std::memory_order_acquire)) {
      status::print("Feed cancelled.");
      return;
    }
    drain_pages(more_results, all_mods);
  }
  if (all_mods.empty()) {
    status::print("Failed to fetch any mods!");
    return;
  }
  deduplicate(all_mods);
  fetch_core_data(all_mods);
  if (quit_requested.load(std::memory_order_acquire)) {
    status::print("Feed cancelled.");
    return;
  }
  status::print("Saving...");
  const std::size_t resolved = static_cast<std::size_t>(
      std::ranges::count_if(all_mods, [](const ModData &mod) {
        return !mod.latest_file_url.empty();
      }));
  if (!store::write_mod_list(cfg::MOD_LIST_FILE.data(), all_mods)) {
    status::print("Fatal: could not write mod list. Aborting pipeline.");
    return;
  }
  std::ranges::sort(all_mods, {}, &ModData::name);
  if (!store::write_mod_list(cfg::BY_NAME_FILE.data(), all_mods)) {
    status::print("Warning: failed to write byname.json");
  }
  std::ranges::sort(all_mods, std::ranges::greater{}, &ModData::latest_file_date);
  if (!store::write_mod_list(cfg::BY_UPDATED_FILE.data(), all_mods)) {
    status::print("Warning: failed to write byupdated.json");
  }
  const int failures = net::failed_requests.load(std::memory_order_acquire);
  if (failures > 0) {
    status::print("Done: {} mods, {} resolved, {} request(s) retried/failed.",
                  all_mods.size(), resolved, failures);
  } else {
    status::print("Done! Enriched {}/{} mods.", resolved, all_mods.size());
  }
}

void thread_main(void *) {
  (void)sd::remove_tree(cfg::LISTS_DIR);
  (void)sd::init_paths();
  net::share_init();
  run_pipeline();
  net::share_cleanup();
  status::mark_feed_done();
}

}  // namespace feed
}  // namespace mm
