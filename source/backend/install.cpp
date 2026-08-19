#include "backend/install.h"

#include "backend/install_archive.h"
#include "backend/net.h"
#include "backend/sd_card.h"
#include "core/config.h"
#include "core/format.h"
#include "core/status.h"
#include "frontend/model.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

namespace mm {
namespace install {

std::atomic<bool> quit_requested{false};
std::atomic<bool> cancel_requested{false};
std::atomic<int> percent{-1};
std::atomic<int64_t> bytes_done{0};
std::atomic<int> files_written{0};
std::vector<std::string> uninstall_files;
std::atomic<int> uninstall_done{0};
std::atomic<bool> uninstalling{false};
std::atomic<int> uninstall_mod_id{0};
std::atomic<bool> uninstall_pending{false};
std::vector<Request> queued_requests;

bool aborting() noexcept {
  return quit_requested.load(std::memory_order_acquire) ||
         cancel_requested.load(std::memory_order_acquire);
}

std::size_t download_write(void *contents, std::size_t size, std::size_t nmemb, void *userp) {
  auto *sink = static_cast<DownloadSink *>(userp);
  std::size_t total = 0;
  if (size != 0 && nmemb != 0) {
    if (size > std::numeric_limits<std::size_t>::max() / nmemb) {
      return CURL_WRITEFUNC_ERROR;
    }
    total = size * nmemb;
  }
  if (total == 0) {
    return 0;
  }
  if (sink->written + static_cast<curl_off_t>(total) > cfg::DOWNLOAD_MAX_BYTES) {
    return CURL_WRITEFUNC_ERROR;
  }
  if (std::fwrite(contents, 1, total, sink->file) != total) {
    return CURL_WRITEFUNC_ERROR;
  }
  sink->written += static_cast<curl_off_t>(total);
  return total;
}

int download_progress(void *, curl_off_t download_total, curl_off_t download_now,
                      curl_off_t, curl_off_t) {
  if (aborting()) {
    return 1;
  }
  percent.store(download_total > 0
                    ? static_cast<int>((download_now * 100) / download_total)
                    : -1,
                std::memory_order_release);
  bytes_done.store(static_cast<int64_t>(download_now), std::memory_order_release);
  return 0;
}

void configure_download(CURL *curl) noexcept {
  net::configure(curl);
  curl_easy_setopt(curl, CURLOPT_SHARE, nullptr);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &download_write);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, nullptr);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, cfg::DL_CONNECT_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, cfg::DL_LOW_SPEED_LIMIT);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, cfg::DL_LOW_SPEED_TIME);
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, cfg::DOWNLOAD_MAX_BYTES);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &download_progress);
}

bool download(CURL *curl, const std::string &url, std::string &message) {
  sd::unlink_quietly(cfg::DOWNLOAD_TMP.data());
  sys::FileHandle file = sd::open(cfg::DOWNLOAD_TMP.data(), "wb");
  if (!file) {
    message = "Cannot write to the SD card.";
    return false;
  }
  DownloadSink sink{file.get(), 0};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
  const CURLcode outcome = curl_easy_perform(curl);
  const bool closed = file.close();
  if (outcome != CURLE_OK) {
    switch (outcome) {
      case CURLE_ABORTED_BY_CALLBACK:
        message = "Cancelled.";
        break;
      case CURLE_OPERATION_TIMEDOUT:
        message = "Download stalled.";
        break;
      case CURLE_WRITE_ERROR:
        message = "Download too large, or the card is full.";
        break;
      default:
        message = fmt::format("Download failed: {}", curl_easy_strerror(outcome));
        break;
    }
    return false;
  }
  if (!closed) {
    message = "SD write failed.";
    return false;
  }
  long code = 0;
  if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) != CURLE_OK ||
      code < 200 || code >= 300) {
    message = fmt::format("Server returned HTTP {}.", code);
    return false;
  }
  curl_off_t reported = 0;
  if (curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &reported) == CURLE_OK &&
      reported != sink.written) {
    message = "Download was interrupted.";
    return false;
  }
  if (sink.written <= 0) {
    message = "The server sent an empty file.";
    return false;
  }
  return true;
}

sys::Mutex mailbox_lock;
sys::Event wake{true};
sys::Thread worker;
Slot slot = Slot::EMPTY;
Request pending;
Result finished;
std::atomic<Phase> phase{Phase::IDLE};
bool ready = false;
std::string user_message;

bool busy() {
  if (!ready) {
    return false;
  }
  const std::scoped_lock guard{mailbox_lock};
  return (slot == Slot::REQUESTED || slot == Slot::RUNNING) || uninstalling.load(std::memory_order_acquire);
}

bool installing(int mod_id) noexcept {
  if (!ready) {
    return false;
  }
  const std::scoped_lock guard{mailbox_lock};
  return (slot == Slot::REQUESTED || slot == Slot::RUNNING) &&
         pending.mod_id == mod_id;
}

bool is_uninstalling() noexcept {
  return uninstalling.load(std::memory_order_acquire);
}

bool is_uninstall_pending() noexcept {
  return uninstall_pending.load(std::memory_order_acquire);
}

void request_uninstall() {
  if (busy() || is_uninstalling() || is_uninstall_pending()) {
    return;
  }
  const store::ModData *mod = model::selected_mod();
  if (mod == nullptr) {
    return;
  }
  if (!store::installed.contains(mod->id)) {
    return;
  }
  uninstall_pending.store(true, std::memory_order_release);
}

void confirm_uninstall() {
  uninstall_pending.store(false, std::memory_order_release);
  if (busy() || is_uninstalling()) {
    return;
  }
  const store::ModData *mod = model::selected_mod();
  if (mod == nullptr) {
    return;
  }
  const store::InstallRecord *record = store::installed.find(mod->id);
  if (record == nullptr) {
    return;
  }
  const int mod_id = mod->id;
  std::vector<std::string> files = record->files;
  store::installed.erase(mod_id);
  if (!store::save_installed()) {
    status::print("Warning: could not save installed mods list.");
  }
  model::resort_after_change();
  if (files.empty()) {
    user_message.clear();
    return;
  }
  begin_uninstall(files, mod_id);
  percent.store(0, std::memory_order_release);
  user_message.clear();
}

void cancel_uninstall_pending() {
  uninstall_pending.store(false, std::memory_order_release);
}

void begin_uninstall(const std::vector<std::string> &files, int mod_id) {
  uninstall_files = files;
  uninstall_done.store(0, std::memory_order_release);
  uninstalling.store(true, std::memory_order_release);
  uninstall_mod_id.store(mod_id, std::memory_order_release);
}

void tick_uninstall() {
  if (!uninstalling.load(std::memory_order_acquire)) {
    return;
  }
  const int total = static_cast<int>(uninstall_files.size());
  const int done = uninstall_done.load(std::memory_order_acquire);
  const int batch = std::max(1, total / 10 + (total % 10 != 0 ? 1 : 0));
  for (int i = 0; i < batch && done + i < total; ++i) {
    const std::string &file = uninstall_files[done + i];
    const std::string path = fmt::format("{}{}", cfg::CTGP7_DIR, file);
    sd::unlink_quietly(path.c_str());
  }
  const int new_done = std::min(done + batch, total);
  uninstall_done.store(new_done, std::memory_order_release);
  percent.store(total > 0 ? (new_done * 100 / total) : 0, std::memory_order_release);
  if (new_done >= total) {
    uninstalling.store(false, std::memory_order_release);
    percent.store(-1, std::memory_order_release);
    user_message.clear();
  }
}

void cancel() {
  if (busy()) {
    cancel_requested.store(true, std::memory_order_release);
  }
}

void worker_main(void *) {
  sys::CurlHandle curl;
  if (curl) {
    configure_download(curl.get());
  }
  for (;;) {
    wake.clear();
    Request request;
    bool have = false;
    {
      const std::scoped_lock guard{mailbox_lock};
      if (!quit_requested.load(std::memory_order_acquire) && slot == Slot::REQUESTED) {
        request = pending;
        slot = Slot::RUNNING;
        have = true;
      } else if (!quit_requested.load(std::memory_order_acquire) && slot == Slot::EMPTY &&
                 !queued_requests.empty()) {
        request = queued_requests.front();
        queued_requests.erase(queued_requests.begin());
        pending = request;
        slot = Slot::REQUESTED;
        have = true;
      }
    }
    if (quit_requested.load(std::memory_order_acquire)) {
      break;
    }
    if (!have) {
      wake.wait_for(cfg::INSTALL_IDLE_WAIT_NS);
      continue;
    }
    Result result;
    result.mod_id = request.mod_id;
    result.file_date = request.file_date;
    result.source_name = request.source_name;
    phase.store(Phase::DOWNLOADING, std::memory_order_release);
    percent.store(-1, std::memory_order_release);
    bytes_done.store(0, std::memory_order_release);
    if (!curl) {
      result.message = "Network is unavailable.";
    } else if (download(curl.get(), request.url, result.message)) {
      phase.store(Phase::EXTRACTING, std::memory_order_release);
      percent.store(-1, std::memory_order_release);
      bytes_done.store(0, std::memory_order_release);
      files_written.store(0, std::memory_order_release);
      ExtractResult extracted;
      if (extract(cfg::DOWNLOAD_TMP.data(), extracted) > 0) {
        result.files = std::move(extracted.files);
        result.ok = true;
      } else if (!extracted.message.empty()) {
        result.message = std::move(extracted.message);
      } else {
        result.message = "No .chpack files.";
      }
    }
    sd::unlink_quietly(cfg::DOWNLOAD_TMP.data());
    phase.store(Phase::FINISHING, std::memory_order_release);
    {
      const std::scoped_lock guard{mailbox_lock};
      finished = std::move(result);
      slot = Slot::COMPLETE;
    }
  }
}

bool begin(const store::ModData &mod) {
  if (!ready || busy()) {
    return false;
  }
  if (mod.latest_file_url.empty()) {
    user_message = "This mod has no download link.";
    return false;
  }
  if (!extension_supported(mod.latest_file_name)) {
    user_message = "Unsupported archive type (only .zip, .7z and .rar).";
    return false;
  }
  Request request;
  request.mod_id = mod.id;
  request.file_date = mod.latest_file_date;
  request.url = mod.latest_file_url;
  request.source_name = mod.latest_file_name;
  cancel_requested.store(false, std::memory_order_release);
  percent.store(-1, std::memory_order_release);
  bytes_done.store(0, std::memory_order_release);
  files_written.store(0, std::memory_order_release);
  phase.store(Phase::DOWNLOADING, std::memory_order_release);
  user_message.clear();
  bool accepted = false;
  {
    const std::scoped_lock guard{mailbox_lock};
    if (slot == Slot::EMPTY) {
      pending = std::move(request);
      slot = Slot::REQUESTED;
      accepted = true;
    }
  }
  if (accepted) {
    wake.signal();
  }
  return accepted;
}

bool queue_mod(const store::ModData &mod) {
  if (!ready) {
    return false;
  }
  if (mod.latest_file_url.empty()) {
    user_message = "This mod has no download link.";
    return false;
  }
  if (!extension_supported(mod.latest_file_name)) {
    user_message = "Unsupported archive type (only .zip, .7z and .rar).";
    return false;
  }
  if (const store::InstallRecord *record = store::installed.find(mod.id)) {
    if (mod.latest_file_date <= record->date) {
      return false;
    }
  }
  Request request;
  request.mod_id = mod.id;
  request.file_date = mod.latest_file_date;
  request.url = mod.latest_file_url;
  request.source_name = mod.latest_file_name;
  bool accepted = false;
  {
    const std::scoped_lock guard{mailbox_lock};
    if (slot == Slot::EMPTY) {
      pending = std::move(request);
      slot = Slot::REQUESTED;
      accepted = true;
    } else {
      const bool duplicate = std::ranges::any_of(
          queued_requests, [&request](const Request &current) {
            return current.mod_id == request.mod_id;
          });
      if (!duplicate && pending.mod_id != request.mod_id) {
        queued_requests.push_back(std::move(request));
        accepted = true;
      }
    }
  }
  if (accepted) {
    wake.signal();
  }
  return accepted;
}

bool queue_selected_mod() {
  const store::ModData *mod = model::selected_mod();
  if (mod == nullptr || model::queued(mod->id)) {
    return false;
  }
  model::queue_selected();
  return queue_mod(*mod);
}

void apply(const Result &result) {
  if (const store::InstallRecord *previous = store::installed.find(result.mod_id)) {
    for (const std::string &old_file : previous->files) {
      const bool still_supplied = std::ranges::any_of(
          result.files, [&old_file](const std::string &current) {
            return equals_ci(old_file, current);
          });
      if (still_supplied) {
        continue;
      }
      const std::string path = fmt::format("{}{}", cfg::CTGP7_DIR, old_file);
      sd::unlink_quietly(path.c_str());
    }
  }
  store::InstallRecord record;
  record.date = result.file_date;
  record.files = result.files;
  record.source_file_name = result.source_name;
  store::installed.insert_or_assign(result.mod_id, std::move(record));
  if (!store::save_installed()) {
    status::print("Warning: could not save installed mods list.");
  }
  model::resort_after_change();
}

void tick() {
  if (!ready) {
    return;
  }
  tick_uninstall();
  Result result;
  bool have = false;
  {
    const std::scoped_lock guard{mailbox_lock};
    if (slot == Slot::COMPLETE) {
      result = std::move(finished);
      finished = Result{};
      slot = Slot::EMPTY;
      have = true;
    }
  }
  if (!have) {
    return;
  }
  phase.store(Phase::IDLE, std::memory_order_release);
  cancel_requested.store(false, std::memory_order_release);
  model::remove_queued_mod(result.mod_id);
  if (result.ok) {
    apply(result);
    user_message.clear();
    return;
  }
  user_message = result.message.empty() ? "Install failed." : std::move(result.message);
}

std::string progress_label() {
  if (is_uninstalling()) {
    const int done = uninstall_done.load(std::memory_order_acquire);
    const int total = static_cast<int>(uninstall_files.size());
    return fmt::format("Removing {}/{}...", done, total);
  }
  switch (phase.load(std::memory_order_acquire)) {
    case Phase::EXTRACTING: {
      const int done = percent.load(std::memory_order_acquire);
      const int files = files_written.load(std::memory_order_acquire);
      if (done < 0) {
        return "Extracting...";
      }
      if (files > 0) {
        return fmt::format("Extracting {}% ({} file{})", done, files,
                           files == 1 ? "" : "s");
      }
      return fmt::format("Extracting {}%", done);
    }
    case Phase::FINISHING:
      return "Finishing...";
    case Phase::IDLE:
    case Phase::DOWNLOADING:
      break;
  }
  const int done = percent.load(std::memory_order_acquire);
  if (done >= 0) {
    return fmt::format("Downloading {}%", std::min(done, 100));
  }
  const int64_t kilobytes = bytes_done.load(std::memory_order_acquire) / 1024;
  if (kilobytes >= 1024) {
    const int64_t whole = kilobytes / 1024;
    const int64_t tenths = ((kilobytes % 1024) * 10) / 1024;
    return fmt::format("Downloading {}.{} MB", whole, tenths);
  }
  return fmt::format("Downloading {} KB", kilobytes);
}

void do_action() {
  if (busy() || is_uninstalling()) {
    return;
  }
  if (!model::queued_mod_ids.empty()) {
    std::vector<int> queued = model::queued_mod_ids;
    for (const int mod_id : queued) {
      const auto it = std::ranges::find_if(model::mods, [mod_id](const store::ModData &mod) {
        return mod.id == mod_id;
      });
      if (it == model::mods.end()) {
        continue;
      }
      const store::InstallRecord *record = store::installed.find(it->id);
      if (record != nullptr && it->latest_file_date <= record->date) {
        model::remove_queued_mod(mod_id);
        continue;
      }
      (void)queue_mod(*it);
    }
    return;
  }
  const store::ModData *mod = model::selected_mod();
  if (mod == nullptr) {
    return;
  }
  const model::ModAction action = model::current_action();
  if (action == model::ModAction::INSTALL || action == model::ModAction::UPDATE) {
    (void)begin(*mod);
  }
}

void uninstall() {
  request_uninstall();
}

bool init() {
  if (ready) {
    return true;
  }
  quit_requested.store(false, std::memory_order_release);
  cancel_requested.store(false, std::memory_order_release);
  slot = Slot::EMPTY;
  sd::unlink_quietly(cfg::DOWNLOAD_TMP.data());
  worker = sys::Thread::spawn(&worker_main, nullptr, cfg::INSTALL_STACK_SIZE,
                              cfg::WORKER_PRIORITY, cfg::ANY_CORE);
  if (!worker) {
    return false;
  }
  ready = true;
  return true;
}

void shutdown() {
  if (!ready) {
    return;
  }
  ready = false;
  quit_requested.store(true, std::memory_order_release);
  wake.signal();
  worker.join();
  uninstalling.store(false, std::memory_order_release);
  sd::unlink_quietly(cfg::DOWNLOAD_TMP.data());
}

}  // namespace install
}  // namespace mm
