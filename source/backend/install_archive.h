#pragma once

#include "core/config.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace mm {
namespace install {

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr bool ends_with_ci(std::string_view text,
                                          std::string_view suffix) noexcept {
  if (suffix.size() > text.size()) {
    return false;
  }
  return std::ranges::equal(text.substr(text.size() - suffix.size()), suffix,
                            [](char a, char b) {
                              return ascii_lower(a) == ascii_lower(b);
                            });
}

[[nodiscard]] constexpr bool equals_ci(std::string_view a, std::string_view b) noexcept {
  return std::ranges::equal(a, b, [](char x, char y) {
    return ascii_lower(x) == ascii_lower(y);
  });
}

[[nodiscard]] constexpr std::string sanitize_entry_name(std::string_view raw) {
  if (raw.empty()) {
    return {};
  }
  const std::size_t separator = raw.find_last_of("/\\");
  const std::string_view name =
      separator == std::string_view::npos ? raw : raw.substr(separator + 1);
  if (name.empty()) {
    return {};
  }
  if (name == "." || name == "..") {
    return {};
  }
  if (name.size() > cfg::INSTALL_NAME_MAX) {
    return {};
  }
  for (const char character : name) {
    const auto value = static_cast<unsigned char>(character);
    if (value < 0x20 || value == 0x7F) {
      return {};
    }
    if (value == ':' || value == '*' || value == '?' || value == '"' ||
        value == '<' || value == '>' || value == '|') {
      return {};
    }
  }
  return std::string{name};
}

[[nodiscard]] bool extension_supported(std::string_view file_name) noexcept;

struct ExtractResult {
  std::vector<std::string> files;
  std::string message;
};

class ArchiveReader {
 public:
  ArchiveReader() noexcept : handle_{archive_read_new()} {}

  ArchiveReader(const ArchiveReader &) = delete;
  ArchiveReader &operator=(const ArchiveReader &) = delete;

  ~ArchiveReader() {
    if (handle_ != nullptr) {
      archive_read_free(handle_);
    }
  }

  [[nodiscard]] struct archive *get() const noexcept {
    return handle_;
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return handle_ != nullptr;
  }

 private:
  struct archive *handle_ = nullptr;
};

[[nodiscard]] std::string archive_message(struct archive *handle, std::string_view what);

[[nodiscard]] bool write_zeros(std::FILE *file, int64_t count) noexcept;

void publish_progress(struct archive *handle, int64_t archive_bytes,
                      std::size_t file_count) noexcept;

[[nodiscard]] std::size_t extract(const char *archive_path, ExtractResult &out);

}  // namespace install
}  // namespace mm
