#pragma once

#include <3ds.h>
#include <curl/curl.h>
#include <json-c/json.h>

#include <concepts>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

namespace mm {
namespace sys {

using CtruThread = ::Thread;

class Mutex {
 public:
  Mutex() noexcept {
    LightLock_Init(&lock_);
  }

  Mutex(const Mutex &) = delete;
  Mutex &operator=(const Mutex &) = delete;

  void lock() noexcept {
    LightLock_Lock(&lock_);
  }

  void unlock() noexcept {
    LightLock_Unlock(&lock_);
  }

  [[nodiscard]] bool try_lock() noexcept {
    return LightLock_TryLock(&lock_) == 0;
  }

 private:
  LightLock lock_{};
};

class RecursiveMutex {
 public:
  RecursiveMutex() noexcept {
    RecursiveLock_Init(&lock_);
  }

  RecursiveMutex(const RecursiveMutex &) = delete;
  RecursiveMutex &operator=(const RecursiveMutex &) = delete;

  void lock() noexcept {
    RecursiveLock_Lock(&lock_);
  }

  void unlock() noexcept {
    RecursiveLock_Unlock(&lock_);
  }

 private:
  RecursiveLock lock_{};
};

class Event {
 public:
  explicit Event(bool sticky = true) noexcept {
    LightEvent_Init(&event_, sticky ? RESET_STICKY : RESET_ONESHOT);
  }

  Event(const Event &) = delete;
  Event &operator=(const Event &) = delete;

  void clear() noexcept {
    LightEvent_Clear(&event_);
  }

  void signal() noexcept {
    LightEvent_Signal(&event_);
  }

  void wait_for(s64 timeout_ns) noexcept {
    LightEvent_WaitTimeout(&event_, timeout_ns);
  }

 private:
  LightEvent event_{};
};

class Thread {
 public:
  Thread() noexcept = default;

  Thread(const Thread &) = delete;
  Thread &operator=(const Thread &) = delete;

  Thread(Thread &&other) noexcept : handle_{std::exchange(other.handle_, nullptr)} {}

  Thread &operator=(Thread &&other) noexcept {
    if (this != &other) {
      join();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  ~Thread() {
    join();
  }

  [[nodiscard]] static Thread spawn(ThreadFunc entry, void *arg, std::size_t stack_size,
                                    int priority, int core) noexcept {
    return Thread{threadCreate(entry, arg, stack_size, priority, core, false)};
  }

  void join() noexcept {
    if (handle_ == nullptr) {
      return;
    }
    threadJoin(handle_, U64_MAX);
    threadFree(handle_);
    handle_ = nullptr;
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return handle_ != nullptr;
  }

 private:
  explicit Thread(CtruThread handle) noexcept : handle_{handle} {}

  CtruThread handle_ = nullptr;
};

struct FreeDeleter {
  void operator()(void *p) const noexcept {
    std::free(p);
  }
};

template <typename T>
using MallocArray = std::unique_ptr<T[], FreeDeleter>;

class LinearBuffer {
 public:
  LinearBuffer() noexcept = default;

  explicit LinearBuffer(std::size_t bytes) noexcept : data_{linearAlloc(bytes)} {}

  LinearBuffer(const LinearBuffer &) = delete;
  LinearBuffer &operator=(const LinearBuffer &) = delete;

  LinearBuffer(LinearBuffer &&other) noexcept : data_{std::exchange(other.data_, nullptr)} {}

  LinearBuffer &operator=(LinearBuffer &&other) noexcept {
    if (this != &other) {
      reset();
      data_ = std::exchange(other.data_, nullptr);
    }
    return *this;
  }

  ~LinearBuffer() {
    reset();
  }

  void reset() noexcept {
    if (data_ == nullptr) {
      return;
    }
    linearFree(data_);
    data_ = nullptr;
  }

  void swap_with(void *&other) noexcept {
    std::swap(data_, other);
  }

  [[nodiscard]] void *get() const noexcept {
    return data_;
  }

  template <typename T>
  [[nodiscard]] T *as() const noexcept {
    return static_cast<T *>(data_);
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return data_ != nullptr;
  }

 private:
  void *data_ = nullptr;
};

class FileHandle {
 public:
  FileHandle() noexcept = default;

  explicit FileHandle(std::FILE *file) noexcept : file_{file} {}

  FileHandle(const FileHandle &) = delete;
  FileHandle &operator=(const FileHandle &) = delete;

  FileHandle(FileHandle &&other) noexcept : file_{std::exchange(other.file_, nullptr)} {}

  FileHandle &operator=(FileHandle &&other) noexcept {
    if (this != &other) {
      (void)close();
      file_ = std::exchange(other.file_, nullptr);
    }
    return *this;
  }

  ~FileHandle() {
    (void)close();
  }

  bool close() noexcept {
    if (file_ == nullptr) {
      return true;
    }
    const bool ok = std::fclose(file_) == 0;
    file_ = nullptr;
    return ok;
  }

  [[nodiscard]] std::FILE *release() noexcept {
    return std::exchange(file_, nullptr);
  }

  [[nodiscard]] std::FILE *get() const noexcept {
    return file_;
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return file_ != nullptr;
  }

 private:
  std::FILE *file_ = nullptr;
};

class CurlHandle {
 public:
  CurlHandle() noexcept : handle_{curl_easy_init()} {}

  CurlHandle(const CurlHandle &) = delete;
  CurlHandle &operator=(const CurlHandle &) = delete;

  ~CurlHandle() {
    if (handle_ != nullptr) {
      curl_easy_cleanup(handle_);
    }
  }

  [[nodiscard]] CURL *get() const noexcept {
    return handle_;
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return handle_ != nullptr;
  }

 private:
  CURL *handle_ = nullptr;
};

class CurlShare {
 public:
  CurlShare() = default;

  CurlShare(const CurlShare &) = delete;
  CurlShare &operator=(const CurlShare &) = delete;

  ~CurlShare() {
    reset();
  }

  bool init(curl_lock_function lock_fn, curl_unlock_function unlock_fn) noexcept {
    reset();
    handle_ = curl_share_init();
    if (handle_ == nullptr) {
      return false;
    }
    curl_share_setopt(handle_, CURLSHOPT_LOCKFUNC, lock_fn);
    curl_share_setopt(handle_, CURLSHOPT_UNLOCKFUNC, unlock_fn);
    curl_share_setopt(handle_, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    return true;
  }

  void reset() noexcept {
    if (handle_ == nullptr) {
      return;
    }
    curl_share_cleanup(handle_);
    handle_ = nullptr;
  }

  [[nodiscard]] CURLSH *get() const noexcept {
    return handle_;
  }

 private:
  CURLSH *handle_ = nullptr;
};

class JsonRef {
 public:
  JsonRef() noexcept = default;

  explicit JsonRef(json_object *object) noexcept : object_{object} {}

  JsonRef(const JsonRef &) = delete;
  JsonRef &operator=(const JsonRef &) = delete;

  JsonRef(JsonRef &&other) noexcept : object_{std::exchange(other.object_, nullptr)} {}

  JsonRef &operator=(JsonRef &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.object_, nullptr));
    }
    return *this;
  }

  ~JsonRef() {
    reset();
  }

  void reset(json_object *object = nullptr) noexcept {
    if (object_ != nullptr) {
      json_object_put(object_);
    }
    object_ = object;
  }

  [[nodiscard]] json_object *release() noexcept {
    return std::exchange(object_, nullptr);
  }

  [[nodiscard]] json_object *get() const noexcept {
    return object_;
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return object_ != nullptr;
  }

 private:
  json_object *object_ = nullptr;
};

template <std::invocable Fn>
class ScopeGuard {
 public:
  explicit ScopeGuard(Fn action) noexcept : action_{std::move(action)} {}

  ScopeGuard(const ScopeGuard &) = delete;
  ScopeGuard &operator=(const ScopeGuard &) = delete;

  ~ScopeGuard() {
    if (armed_) {
      action_();
    }
  }

  void dismiss() noexcept {
    armed_ = false;
  }

 private:
  Fn action_;
  bool armed_ = true;
};

template <std::invocable Fn>
ScopeGuard(Fn) -> ScopeGuard<Fn>;

template <typename Fn>
auto make_scope_guard(Fn &&fn) {
  return ScopeGuard<std::decay_t<Fn>>{std::forward<Fn>(fn)};
}

}  // namespace sys
}  // namespace mm
