#pragma once

#include "backend/store.h"
#include "core/system.h"

#include <curl/curl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mm {
namespace install {

extern std::atomic<bool> quit_requested;
extern std::atomic<bool> cancel_requested;
extern std::atomic<int> percent;
extern std::atomic<int64_t> bytes_done;
extern std::atomic<int> files_written;
extern std::vector<std::string> uninstall_files;
extern std::atomic<int> uninstall_done;
extern std::atomic<bool> uninstalling;
extern std::atomic<int> uninstall_mod_id;
extern std::atomic<bool> uninstall_pending;

[[nodiscard]] bool aborting() noexcept;

struct DownloadSink {
  std::FILE *file = nullptr;
  curl_off_t written = 0;
};

std::size_t download_write(void *contents, std::size_t size, std::size_t nmemb, void *userp);

int download_progress(void *, curl_off_t download_total, curl_off_t download_now,
                      curl_off_t, curl_off_t);

void configure_download(CURL *curl) noexcept;

[[nodiscard]] bool download(CURL *curl, const std::string &url, std::string &message);

enum class Phase {
  IDLE,
  DOWNLOADING,
  EXTRACTING,
  FINISHING
};

enum class Slot {
  EMPTY,
  REQUESTED,
  RUNNING,
  COMPLETE
};

struct Request {
  int mod_id = 0;
  int64_t file_date = 0;
  std::string url;
  std::string source_name;
};

struct Result {
  int mod_id = 0;
  int64_t file_date = 0;
  std::string source_name;
  std::vector<std::string> files;
  bool ok = false;
  std::string message;
};

extern sys::Mutex mailbox_lock;
extern sys::Event wake;
extern sys::Thread worker;
extern Slot slot;
extern Request pending;
extern Result finished;
extern std::atomic<Phase> phase;
extern bool ready;
extern std::string user_message;

[[nodiscard]] bool busy();

[[nodiscard]] bool installing(int mod_id) noexcept;

[[nodiscard]] bool is_uninstalling() noexcept;

[[nodiscard]] bool is_uninstall_pending() noexcept;

void cancel();

void worker_main(void *);

[[nodiscard]] bool begin(const store::ModData &mod);

[[nodiscard]] bool queue_mod(const store::ModData &mod);

[[nodiscard]] bool queue_selected_mod();

void apply(const Result &result);

void tick();

[[nodiscard]] std::string progress_label();

void do_action();

void uninstall();

void request_uninstall();

void confirm_uninstall();

void cancel_uninstall_pending();

void begin_uninstall(const std::vector<std::string> &files, int mod_id);

void tick_uninstall();

bool init();

void shutdown();

}  // namespace install
}  // namespace mm
