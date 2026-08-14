#include "backend/install.h"

#include "backend/install_archive.h"
#include "backend/net.h"
#include "backend/sd_card.h"
#include "core/config.h"
#include "core/format.h"
#include "frontend/model.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace mm {
namespace install {

std::atomic<bool> quit_requested{false};
std::atomic<bool> cancel_requested{false};
std::atomic<int> percent{-1};
std::atomic<int64_t> bytes_done{0};
std::atomic<int> files_written{0};

bool aborting() noexcept {
  return quit_requested.load(std::memory_order_relaxed) ||
         cancel_requested.load(std::memory_order_relaxed);
}

std::size_t download_write(void *contents, std::size_t size, std::size_t nmemb, void *userp) {
  auto *sink = static_cast<DownloadSink *>(userp);
  const std::size_t total = size * nmemb;
  if (total == 0) {
    return 0;
  }
  if (sink->written > cfg::DOWNLOAD_MAX_BYTES - static_cast<curl_off_t>(total)) {
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
                std::memory_order_relaxed);
  bytes_done.store(static_cast<int64_t>(download_now), std::memory_order_relaxed);
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
  return slot == Slot::REQUESTED || slot == Slot::RUNNING;
}

void cancel() {
  if (busy()) {
    cancel_requested.store(true, std::memory_order_relaxed);
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
      if (!quit_requested.load(std::memory_order_relaxed) && slot == Slot::REQUESTED) {
        request = pending;
        slot = Slot::RUNNING;
        have = true;
      }
    }
    if (quit_requested.load(std::memory_order_relaxed)) {
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
    phase.store(Phase::DOWNLOADING, std::memory_order_relaxed);
    percent.store(-1, std::memory_order_relaxed);
    bytes_done.store(0, std::memory_order_relaxed);
    if (!curl) {
      result.message = "Network is unavailable.";
    } else if (download(curl.get(), request.url, result.message)) {
      phase.store(Phase::EXTRACTING, std::memory_order_relaxed);
      percent.store(-1, std::memory_order_relaxed);
      bytes_done.store(0, std::memory_order_relaxed);
      files_written.store(0, std::memory_order_relaxed);
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
    phase.store(Phase::FINISHING, std::memory_order_relaxed);
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
  cancel_requested.store(false, std::memory_order_relaxed);
  percent.store(-1, std::memory_order_relaxed);
  bytes_done.store(0, std::memory_order_relaxed);
  files_written.store(0, std::memory_order_relaxed);
  phase.store(Phase::DOWNLOADING, std::memory_order_relaxed);
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
  (void)store::save_installed();
  model::resort_after_change();
}

void tick() {
  if (!ready) {
    return;
  }
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
  phase.store(Phase::IDLE, std::memory_order_relaxed);
  cancel_requested.store(false, std::memory_order_relaxed);
  if (result.ok) {
    apply(result);
    user_message.clear();
    return;
  }
  user_message = result.message.empty() ? "Install failed." : std::move(result.message);
}

std::string progress_label() {
  switch (phase.load(std::memory_order_relaxed)) {
    case Phase::EXTRACTING: {
      const int done = percent.load(std::memory_order_relaxed);
      const int files = files_written.load(std::memory_order_relaxed);
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
  const int done = percent.load(std::memory_order_relaxed);
  if (done >= 0) {
    return fmt::format("Downloading {}%", std::min(done, 100));
  }
  const int64_t kilobytes = bytes_done.load(std::memory_order_relaxed) / 1024;
  if (kilobytes >= 1024) {
    const int64_t whole = kilobytes / 1024;
    const int64_t tenths = ((kilobytes % 1024) * 10) / 1024;
    return fmt::format("Downloading {}.{} MB", whole, tenths);
  }
  return fmt::format("Downloading {} KB", kilobytes);
}

void do_action() {
  if (busy()) {
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
  if (busy()) {
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
  const std::vector<std::string> files = record->files;
  const int mod_id = mod->id;
  store::installed.erase(mod_id);
  (void)store::save_installed();
  for (const std::string &file : files) {
    const std::string path = fmt::format("{}{}", cfg::CTGP7_DIR, file);
    sd::unlink_quietly(path.c_str());
  }
  user_message.clear();
  model::resort_after_change();
}

bool init() {
  if (ready) {
    return true;
  }
  quit_requested.store(false, std::memory_order_relaxed);
  cancel_requested.store(false, std::memory_order_relaxed);
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
  quit_requested.store(true, std::memory_order_relaxed);
  wake.signal();
  worker.join();
  sd::unlink_quietly(cfg::DOWNLOAD_TMP.data());
}

}  // namespace install
}  // namespace mm
