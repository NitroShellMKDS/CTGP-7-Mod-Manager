#include "backend/install_archive.h"

#include "backend/install.h"
#include "backend/sd_card.h"
#include "core/format.h"
#include "core/system.h"

#include <array>
#include <atomic>

namespace mm {
namespace install {

namespace {

[[nodiscard]] consteval bool sanitize_entry_name_is_safe() {
  const auto rejects = [](std::string_view input) {
    return sanitize_entry_name(input).empty();
  };
  const auto yields = [](std::string_view input, std::string_view expected) {
    return sanitize_entry_name(input) == expected;
  };
  if (!yields("mario.chpack", "mario.chpack")) {
    return false;
  }
  if (!yields("sub/dir/mario.chpack", "mario.chpack")) {
    return false;
  }
  if (!yields("..\\..\\boot.firm", "boot.firm")) {
    return false;
  }
  if (!yields("a/b\\c/evil.chpack", "evil.chpack")) {
    return false;
  }
  if (!rejects("")) {
    return false;
  }
  if (!rejects(".")) {
    return false;
  }
  if (!rejects("..")) {
    return false;
  }
  if (!rejects("dir/")) {
    return false;
  }
  if (!rejects("sdmc:evil")) {
    return false;
  }
  if (!rejects("C:evil")) {
    return false;
  }
  if (!rejects("bad*name")) {
    return false;
  }
  if (!rejects("bad?name")) {
    return false;
  }
  if (!rejects("bad|name")) {
    return false;
  }
  if (!rejects("bad\"name")) {
    return false;
  }
  if (!rejects("bad<name")) {
    return false;
  }
  if (!rejects("bad>name")) {
    return false;
  }
  if (!rejects("ctrl\x01name")) {
    return false;
  }
  if (!rejects("del\x7Fname")) {
    return false;
  }
  const std::string overlong(cfg::INSTALL_NAME_MAX + 1, 'x');
  return sanitize_entry_name(overlong).empty();
}

static_assert(sanitize_entry_name_is_safe(),
              "Archive entry names must be reduced to a safe bare filename.");

}  // namespace

bool extension_supported(std::string_view file_name) noexcept {
  return ends_with_ci(file_name, ".zip") ||
         ends_with_ci(file_name, ".7z") ||
         ends_with_ci(file_name, ".rar");
}

std::string archive_message(struct archive *handle, std::string_view what) {
  const char *detail = handle != nullptr ? archive_error_string(handle) : nullptr;
  if (detail != nullptr && detail[0] != '\0') {
    return fmt::format("{}: {}", what, detail);
  }
  return fmt::format("{}.", what);
}

bool write_zeros(std::FILE *file, int64_t count) noexcept {
  constexpr std::size_t CHUNK = 512;
  const std::array<char, CHUNK> zeros{};
  while (count > 0) {
    const auto chunk = static_cast<std::size_t>(std::min<int64_t>(count, CHUNK));
    if (std::fwrite(zeros.data(), 1, chunk, file) != chunk) {
      return false;
    }
    count -= static_cast<int64_t>(chunk);
  }
  return true;
}

void publish_progress(struct archive *handle, int64_t archive_bytes,
                      std::size_t file_count) noexcept {
  files_written.store(static_cast<int>(file_count), std::memory_order_relaxed);
  if (archive_bytes <= 0) {
    return;
  }
  const la_int64_t consumed = archive_filter_bytes(handle, -1);
  if (consumed < 0) {
    return;
  }
  const int64_t ratio = (static_cast<int64_t>(consumed) * 100) / archive_bytes;
  percent.store(static_cast<int>(std::clamp<int64_t>(ratio, 0, 100)),
                std::memory_order_relaxed);
}

std::size_t extract(const char *archive_path, ExtractResult &out) {
  ArchiveReader reader;
  if (!reader) {
    out.message = "Out of memory.";
    return 0;
  }
  archive_read_support_format_zip(reader.get());
  archive_read_support_format_7zip(reader.get());
  archive_read_support_format_rar(reader.get());
  archive_read_support_format_rar5(reader.get());
  const int64_t archive_bytes = sd::file_size(archive_path).value_or(0);
  {
    const sd::PathGuard guard{sd::path_lock};
    if (archive_read_open_filename(reader.get(), archive_path,
                                   cfg::ARCHIVE_BLOCK_SIZE) != ARCHIVE_OK) {
      out.message = archive_message(reader.get(), "Cannot open archive");
      return 0;
    }
  }
  sys::ScopeGuard discard{[&out] {
    out.files.clear();
  }};
  int64_t total_bytes = 0;
  int entries = 0;
  for (;;) {
    if (aborting()) [[unlikely]] {
      out.message = "Cancelled.";
      return 0;
    }
    if (++entries > cfg::INSTALL_MAX_ENTRIES) {
      out.message = "Archive has too many entries.";
      return 0;
    }
    struct archive_entry *entry = nullptr;
    const int header = archive_read_next_header(reader.get(), &entry);
    if (header == ARCHIVE_EOF) {
      break;
    }
    if (header == ARCHIVE_RETRY) {
      continue;
    }
    if (header < ARCHIVE_WARN) {
      out.message = archive_message(reader.get(), "Archive error");
      return 0;
    }
    publish_progress(reader.get(), archive_bytes, out.files.size());
    if (archive_entry_filetype(entry) != AE_IFREG) {
      continue;
    }
    if (archive_entry_is_encrypted(entry)) {
      out.message = "Archive is password protected.";
      return 0;
    }
    const char *raw = archive_entry_pathname_utf8(entry);
    if (raw == nullptr) {
      raw = archive_entry_pathname(entry);
    }
    if (raw == nullptr) {
      continue;
    }
    if (!ends_with_ci(raw, cfg::CHPACK_SUFFIX)) {
      continue;
    }
    const std::string name = sanitize_entry_name(raw);
    if (name.empty()) {
      continue;
    }
    const bool already_recorded =
        std::ranges::any_of(out.files, [&name](const std::string &seen) {
          return equals_ci(seen, name);
        });
    if (!already_recorded && out.files.size() >= cfg::INSTALL_MAX_FILES) {
      out.message = "Archive has too many .chpack files.";
      return 0;
    }
    const std::string destination = fmt::format("{}{}", cfg::CTGP7_DIR.view(), name);
    const std::string partial = destination + ".part";
    sys::FileHandle file = sd::open(partial.c_str(), "wb");
    if (!file) {
      out.message = "Cannot write to the SD card.";
      return 0;
    }
    sys::ScopeGuard remove_partial{[&partial] {
      sd::unlink_quietly(partial.c_str());
    }};
    int64_t expected_offset = 0;
    for (;;) {
      const void *block = nullptr;
      std::size_t block_size = 0;
      la_int64_t block_offset = 0;
      const int status = archive_read_data_block(reader.get(), &block, &block_size,
                                                 &block_offset);
      if (status == ARCHIVE_EOF) {
        break;
      }
      if (status < ARCHIVE_WARN) {
        out.message = archive_message(reader.get(), "Extract failed");
        return 0;
      }
      if (block_size == 0) {
        continue;
      }
      if (aborting()) [[unlikely]] {
        out.message = "Cancelled.";
        return 0;
      }
      const auto offset = static_cast<int64_t>(block_offset);
      if (offset < expected_offset) {
        out.message = "Corrupt archive entry.";
        return 0;
      }
      if (offset > expected_offset) {
        if (!write_zeros(file.get(), offset - expected_offset)) {
          out.message = "SD write failed.";
          return 0;
        }
        expected_offset = offset;
      }
      expected_offset += static_cast<int64_t>(block_size);
      total_bytes += static_cast<int64_t>(block_size);
      if (expected_offset > cfg::INSTALL_MAX_FILE_BYTES ||
          total_bytes > cfg::INSTALL_MAX_TOTAL_BYTES) {
        out.message = "Archive is too large.";
        return 0;
      }
      if (std::fwrite(block, 1, block_size, file.get()) != block_size) {
        out.message = "SD write failed (card full?).";
        return 0;
      }
      bytes_done.store(total_bytes, std::memory_order_relaxed);
      publish_progress(reader.get(), archive_bytes, out.files.size());
    }
    if (!file.close()) {
      out.message = "SD write failed.";
      return 0;
    }
    if (!sd::replace_file(partial.c_str(), destination.c_str())) {
      out.message = "Cannot replace the installed file.";
      return 0;
    }
    remove_partial.dismiss();
    if (!already_recorded) {
      out.files.push_back(name);
    }
    publish_progress(reader.get(), archive_bytes, out.files.size());
  }
  discard.dismiss();
  return out.files.size();
}

}  // namespace install
}  // namespace mm
