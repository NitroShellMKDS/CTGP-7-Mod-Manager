#pragma once

#include "core/config.h"
#include "core/system.h"

#include <3ds.h>
#include <curl/curl.h>
#include <setjmp.h>
#include <jpeglib.h>

#include <atomic>
#include <cstddef>
#include <optional>
#include <string>

namespace mm {
namespace thumbs {

struct RawImage {
  unsigned char *rgb = nullptr;
  int width = 0;
  int height = 0;
};

struct JpegError {
  jpeg_error_mgr pub;
  jmp_buf jump;
};

void jpeg_error_exit(j_common_ptr info);

void jpeg_silence(j_common_ptr);

[[nodiscard]] constexpr unsigned pick_scale_numerator(unsigned width, unsigned height) noexcept {
  for (unsigned numerator = 1; numerator < 8; ++numerator) {
    if ((width * numerator + 7u) / 8u >= static_cast<unsigned>(cfg::THUMB_IMG_W) &&
        (height * numerator + 7u) / 8u >= static_cast<unsigned>(cfg::THUMB_IMG_H)) {
      return numerator;
    }
  }
  return 8;
}

[[nodiscard]] bool jpeg_decode(const unsigned char *source, std::size_t length, RawImage &out);

[[nodiscard]] bool jpeg_encode(const unsigned char *rgb, unsigned char **out_buffer,
                               unsigned long *out_length);

void crop_resize(const RawImage &source, unsigned char *destination) noexcept;

[[nodiscard]] constexpr u32 tiled_index(u32 x, u32 y) noexcept {
  const u32 tile_row = ((y >> 3) * (cfg::THUMB_TEX_W >> 3)) << 6;
  const u32 tile_col = (x >> 3) << 6;
  const u32 x_bits = (x & 1u) | ((x & 2u) << 1) | ((x & 4u) << 2);
  const u32 y_bits = ((y & 1u) << 1) | ((y & 2u) << 2) | ((y & 4u) << 3);
  return tile_row + tile_col + x_bits + y_bits;
}

void swizzle_rgb565(const unsigned char *source, int source_width, int source_height,
                    u16 *destination) noexcept;

struct Buffer {
  sys::MallocArray<unsigned char> data;
  std::size_t length = 0;
};

[[nodiscard]] std::string cache_path(int mod_id);

[[nodiscard]] std::optional<Buffer> read_cached(const char *path);

[[nodiscard]] bool write_cached(const std::string &path, const void *data, std::size_t length);

struct DownloadBuffer {
  unsigned char *data = nullptr;
  std::size_t length = 0;
  std::size_t capacity = 0;
};

extern std::atomic<bool> quit_requested;

std::size_t download_write(void *contents, std::size_t size, std::size_t nmemb, void *userp);

int download_abort(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t);

void configure_download(CURL *curl) noexcept;

[[nodiscard]] bool produce(CURL *curl, int mod_id, const char *url, u16 *destination);

}  // namespace thumbs
}  // namespace mm
