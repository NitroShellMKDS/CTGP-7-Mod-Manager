#pragma once

#include "core/system.h"

#include <dirent.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace mm {
namespace sd {

extern sys::RecursiveMutex path_lock;

using PathGuard = std::scoped_lock<sys::RecursiveMutex>;

class DirHandle {
 public:
  explicit DirHandle(const char *path) noexcept : dir_{::opendir(path)} {}

  DirHandle(const DirHandle &) = delete;
  DirHandle &operator=(const DirHandle &) = delete;

  ~DirHandle() {
    if (dir_ != nullptr) {
      ::closedir(dir_);
    }
  }

  [[nodiscard]] DIR *get() const noexcept {
    return dir_;
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return dir_ != nullptr;
  }

 private:
  DIR *dir_ = nullptr;
};

[[nodiscard]] std::string join(std::string_view directory, std::string_view name);

[[nodiscard]] bool remove_tree(std::string_view root);

[[nodiscard]] bool make_directories(std::string_view path);

void unlink_quietly(const char *path) noexcept;

[[nodiscard]] bool replace_file(const char *source, const char *destination) noexcept;

[[nodiscard]] std::optional<int64_t> file_size(const char *path) noexcept;

[[nodiscard]] bool exists(const char *path) noexcept;

[[nodiscard]] sys::FileHandle open(const char *path, const char *mode) noexcept;

[[nodiscard]] bool init_paths();

}  // namespace sd
}  // namespace mm
