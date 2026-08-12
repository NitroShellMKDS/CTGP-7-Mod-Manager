#include "backend/net.h"

#include <3ds.h>

namespace mm {
namespace net {

std::atomic<int> failed_requests{0};
std::array<sys::Mutex, CURL_LOCK_DATA_LAST> share_locks;
sys::CurlShare share;

void share_lock_cb(CURL *, curl_lock_data data, curl_lock_access, void *) {
  if (data < CURL_LOCK_DATA_LAST) {
    share_locks[static_cast<std::size_t>(data)].lock();
  }
}

void share_unlock_cb(CURL *, curl_lock_data data, void *) {
  if (data < CURL_LOCK_DATA_LAST) {
    share_locks[static_cast<std::size_t>(data)].unlock();
  }
}

void share_init() {
  (void)share.init(&share_lock_cb, &share_unlock_cb);
}

void share_cleanup() {
  share.reset();
}

std::size_t write_to_string(void *contents, std::size_t size, std::size_t nmemb, void *userp) {
  const std::size_t total = size * nmemb;
  auto *out = static_cast<std::string *>(userp);
  if (total > 0 && out->size() > cfg::MAX_RESPONSE_SIZE - total) {
    return CURL_WRITEFUNC_ERROR;
  }
  if (total == 0) {
    return 0;
  }
  out->append(static_cast<const char *>(contents), total);
  return total;
}

void configure(CURL *curl) noexcept {
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_to_string);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, cfg::USER_AGENT);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CAINFO, cfg::CA_BUNDLE_PATH);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (share.get() != nullptr) {
    curl_easy_setopt(curl, CURLOPT_SHARE, share.get());
  }
}

Response get(CURL *curl, const std::string &url) {
  Response response;
  if (curl == nullptr) {
    return response;
  }
  response.data.reserve(cfg::RESPONSE_RESERVE);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.data);
  if (curl_easy_perform(curl) == CURLE_OK &&
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code) == CURLE_OK) {
    response.ok = response.status_code >= 200 && response.status_code < 300;
  }
  if (!response.ok) {
    failed_requests.fetch_add(1, std::memory_order_relaxed);
    response.data.clear();
  }
  return response;
}

Response get_with_retry(CURL *curl, const std::string &url, int max_attempts) {
  Response response;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (attempt > 0) {
      svcSleepThread(cfg::RETRY_BASE_DELAY_NS << (attempt - 1));
    }
    response = get(curl, url);
    if (response.ok) {
      return response;
    }
    if (request_too_large(response.status_code)) {
      break;
    }
    if (response.status_code == 429 || response.status_code == 503) {
      svcSleepThread(cfg::THROTTLE_DELAY_NS);
    }
  }
  return response;
}

}  // namespace net
}  // namespace mm
