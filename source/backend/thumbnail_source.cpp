#include "backend/thumbnail_source.h"

#include "backend/net.h"
#include "backend/sd_card.h"
#include "core/format.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {
namespace thumbs {

namespace {

[[nodiscard]] consteval bool tiled_index_is_bijective() {
  constexpr std::size_t TEXELS = static_cast<std::size_t>(cfg::THUMB_TEX_W) *
                                 static_cast<std::size_t>(cfg::THUMB_TEX_H);
  std::array<bool, TEXELS> seen{};
  for (u32 y = 0; y < cfg::THUMB_TEX_H; ++y) {
    for (u32 x = 0; x < cfg::THUMB_TEX_W; ++x) {
      const u32 index = tiled_index(x, y);
      if (index >= TEXELS || seen[index]) {
        return false;
      }
      seen[index] = true;
    }
  }
  return true;
}

static_assert(tiled_index_is_bijective(),
              "The tiled texture index must cover every texel exactly once.");

}  // namespace

static_assert(pick_scale_numerator(220, 124) == 4,
              "220x124 should decode at 4/8 scale.");
static_assert(pick_scale_numerator(110, 62) == 8,
              "A 110x62 source must not be downscaled.");

void jpeg_error_exit(j_common_ptr info) {
  longjmp(reinterpret_cast<JpegError *>(info->err)->jump, 1);
}

void jpeg_silence(j_common_ptr) {}

bool jpeg_decode(const unsigned char *source, std::size_t length, RawImage &out) {
  jpeg_decompress_struct cinfo;
  JpegError jerr;
  unsigned char *volatile rgb = nullptr;
  volatile bool ok = false;
  volatile int width = 0;
  volatile int height = 0;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = &jpeg_error_exit;
  jerr.pub.output_message = &jpeg_silence;
  if (setjmp(jerr.jump) == 0) {
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, source, static_cast<unsigned long>(length));
    if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK &&
        cinfo.image_width > 0 &&
        cinfo.image_width <= cfg::THUMB_SRC_MAX_DIM &&
        cinfo.image_height > 0 &&
        cinfo.image_height <= cfg::THUMB_SRC_MAX_DIM) {
      cinfo.scale_num = pick_scale_numerator(cinfo.image_width, cinfo.image_height);
      cinfo.scale_denom = 8;
      cinfo.out_color_space = JCS_RGB;
      cinfo.dct_method = JDCT_IFAST;
      cinfo.do_fancy_upsampling = FALSE;
      jpeg_calc_output_dimensions(&cinfo);
      const std::size_t pixels =
          static_cast<std::size_t>(cinfo.output_width) * cinfo.output_height;
      if (cinfo.output_components == 3 && pixels <= cfg::THUMB_SRC_MAX_PIXELS) {
        rgb = static_cast<unsigned char *>(std::malloc(pixels * 3u));
        if (rgb != nullptr) {
          jpeg_start_decompress(&cinfo);
          while (cinfo.output_scanline < cinfo.output_height) {
            JSAMPROW row = rgb + static_cast<std::size_t>(cinfo.output_scanline) *
                                     static_cast<std::size_t>(cinfo.output_width) * 3u;
            if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) {
              break;
            }
          }
          ok = cinfo.output_scanline >= cinfo.output_height;
          width = static_cast<int>(cinfo.output_width);
          height = static_cast<int>(cinfo.output_height);
          jpeg_finish_decompress(&cinfo);
        }
      }
    }
  }
  jpeg_destroy_decompress(&cinfo);
  if (!ok) {
    std::free(rgb);
    return false;
  }
  out.rgb = rgb;
  out.width = width;
  out.height = height;
  return true;
}

bool jpeg_encode(unsigned char *rgb, unsigned char **out_buffer,
                 unsigned long *out_length) {
  jpeg_compress_struct cinfo;
  JpegError jerr;
  unsigned char *buffer = nullptr;
  unsigned long length = 0;
  volatile bool ok = false;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = &jpeg_error_exit;
  jerr.pub.output_message = &jpeg_silence;
  if (setjmp(jerr.jump) == 0) {
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &buffer, &length);
    cinfo.image_width = static_cast<JDIMENSION>(cfg::THUMB_IMG_W);
    cinfo.image_height = static_cast<JDIMENSION>(cfg::THUMB_IMG_H);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, cfg::THUMB_JPEG_QUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
      JSAMPROW row = rgb + static_cast<std::size_t>(cinfo.next_scanline) *
                               static_cast<std::size_t>(cfg::THUMB_IMG_W) * 3u;
      if (jpeg_write_scanlines(&cinfo, &row, 1) != 1) {
        break;
      }
    }
    if (cinfo.next_scanline >= cinfo.image_height) {
      jpeg_finish_compress(&cinfo);
      ok = true;
    }
  }
  jpeg_destroy_compress(&cinfo);
  if (!ok || buffer == nullptr) {
    std::free(buffer);
    return false;
  }
  *out_buffer = buffer;
  *out_length = length;
  return true;
}

void crop_resize(const RawImage &source, unsigned char *destination) noexcept {
  const int source_width = source.width;
  const int source_height = source.height;
  if (source_width <= 0 || source_height <= 0) {
    return;
  }
  int crop_width = 0;
  int crop_height = 0;
  if (source_width * cfg::THUMB_IMG_H >= source_height * cfg::THUMB_IMG_W) {
    crop_width = (source_height * cfg::THUMB_IMG_W) / cfg::THUMB_IMG_H;
    crop_height = source_height;
  } else {
    crop_width = source_width;
    crop_height = (source_width * cfg::THUMB_IMG_H) / cfg::THUMB_IMG_W;
  }
  crop_width = std::max(crop_width, 1);
  crop_height = std::max(crop_height, 1);
  int crop_x = std::max((source_width - crop_width) / 2, 0);
  crop_width = std::min(crop_width, source_width - crop_x);
  crop_height = std::min(crop_height, source_height);
  for (int y = 0; y < cfg::THUMB_IMG_H; ++y) {
    int y0 = (y * crop_height) / cfg::THUMB_IMG_H;
    int y1 = ((y + 1) * crop_height) / cfg::THUMB_IMG_H;
    if (y1 <= y0) {
      y1 = y0 + 1;
    }
    y1 = std::min(y1, crop_height);
    for (int x = 0; x < cfg::THUMB_IMG_W; ++x) {
      int x0 = crop_x + (x * crop_width) / cfg::THUMB_IMG_W;
      int x1 = crop_x + ((x + 1) * crop_width) / cfg::THUMB_IMG_W;
      if (x1 <= x0) {
        x1 = x0 + 1;
      }
      x1 = std::min(x1, crop_x + crop_width);
      unsigned red = 0;
      unsigned green = 0;
      unsigned blue = 0;
      unsigned samples = 0;
      for (int sy = y0; sy < y1; ++sy) {
        const unsigned char *pixel =
            source.rgb + (static_cast<std::size_t>(sy) *
                              static_cast<std::size_t>(source_width) +
                          static_cast<std::size_t>(x0)) *
                             3u;
        for (int sx = x0; sx < x1; ++sx, pixel += 3) {
          red += pixel[0];
          green += pixel[1];
          blue += pixel[2];
          ++samples;
        }
      }
      unsigned char *out =
          destination + (static_cast<std::size_t>(y) *
                             static_cast<std::size_t>(cfg::THUMB_IMG_W) +
                         static_cast<std::size_t>(x)) *
                            3u;
      out[0] = static_cast<unsigned char>(red / samples);
      out[1] = static_cast<unsigned char>(green / samples);
      out[2] = static_cast<unsigned char>(blue / samples);
    }
  }
}

void swizzle_rgb565(const unsigned char *source, int source_width, int source_height,
                    u16 *destination) noexcept {
  for (u32 y = 0; y < cfg::THUMB_TEX_H; ++y) {
    const u32 sy = y < static_cast<u32>(source_height)
                       ? y
                       : static_cast<u32>(source_height - 1);
    const unsigned char *row = source + static_cast<std::size_t>(sy) *
                                            static_cast<std::size_t>(source_width) * 3u;
    for (u32 x = 0; x < cfg::THUMB_TEX_W; ++x) {
      const u32 sx = x < static_cast<u32>(source_width)
                         ? x
                         : static_cast<u32>(source_width - 1);
      const unsigned char *pixel = row + static_cast<std::size_t>(sx) * 3u;
      const auto texel = static_cast<u16>(
          ((static_cast<u32>(pixel[0]) & 0xF8u) << 8) |
          ((static_cast<u32>(pixel[1]) & 0xFCu) << 3) |
          (static_cast<u32>(pixel[2]) >> 3));
      destination[tiled_index(x, y)] = texel;
    }
  }
}

std::string cache_path(int mod_id) {
  return fmt::format("{}{}.jpg", cfg::THUMB_DIR, mod_id);
}

std::optional<Buffer> read_cached(const char *path) {
  sys::FileHandle file = sd::open(path, "rb");
  if (!file) {
    return std::nullopt;
  }
  if (std::fseek(file.get(), 0, SEEK_END) != 0) {
    return std::nullopt;
  }
  const long size = std::ftell(file.get());
  if (size <= 0 || static_cast<std::size_t>(size) > cfg::THUMB_MAX_BYTES) {
    return std::nullopt;
  }
  std::rewind(file.get());
  const auto length = static_cast<std::size_t>(size);
  Buffer buffer{
      sys::MallocArray<unsigned char>{
          static_cast<unsigned char *>(std::malloc(length))},
      length};
  if (!buffer.data) {
    return std::nullopt;
  }
  if (std::fread(buffer.data.get(), 1, length, file.get()) != length) {
    return std::nullopt;
  }
  return buffer;
}

bool write_cached(const std::string &path, const void *data, std::size_t length) {
  const std::string temporary = path + ".tmp";
  {
    sys::FileHandle file = sd::open(temporary.c_str(), "wb");
    if (!file) {
      return false;
    }
    const bool wrote = std::fwrite(data, 1, length, file.get()) == length;
    const bool closed = file.close();
    if (!wrote || !closed) {
      sd::unlink_quietly(temporary.c_str());
      return false;
    }
  }
  return sd::replace_file(temporary.c_str(), path.c_str());
}

std::atomic<bool> quit_requested{false};

std::size_t download_write(void *contents, std::size_t size, std::size_t nmemb, void *userp) {
  auto *sink = static_cast<DownloadBuffer *>(userp);
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
  if (sink->length + total > cfg::THUMB_MAX_BYTES) {
    return CURL_WRITEFUNC_ERROR;
  }
  if (sink->length + total > sink->capacity) {
    std::size_t capacity = sink->capacity != 0 ? sink->capacity : 32768;
    while (capacity < sink->length + total) {
      capacity *= 2;
    }
    auto *grown = static_cast<unsigned char *>(std::realloc(sink->data, capacity));
    if (grown == nullptr) {
      return CURL_WRITEFUNC_ERROR;
    }
    sink->data = grown;
    sink->capacity = capacity;
  }
  std::memcpy(sink->data + sink->length, contents, total);
  sink->length += total;
  return total;
}

int download_abort(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return quit_requested.load(std::memory_order_acquire) ? 1 : 0;
}

void configure_download(CURL *curl) noexcept {
  net::configure(curl);
  curl_easy_setopt(curl, CURLOPT_SHARE, nullptr);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &download_write);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, nullptr);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &download_abort);
}

bool produce(CURL *curl, int mod_id, const char *url, u16 *destination) {
  const std::string path = cache_path(mod_id);
  RawImage image{};
  bool from_disk = false;
  if (std::optional<Buffer> cached = read_cached(path.c_str())) {
    if (jpeg_decode(cached->data.get(), cached->length, image)) {
      from_disk = true;
    } else {
      sd::unlink_quietly(path.c_str());
    }
  }
  if (!from_disk) {
    if (curl == nullptr || url[0] == '\0' ||
        quit_requested.load(std::memory_order_acquire)) {
      return false;
    }
    DownloadBuffer sink;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    const CURLcode outcome = curl_easy_perform(curl);
    const sys::MallocArray<unsigned char> downloaded{sink.data};
    if (outcome != CURLE_OK) {
      return false;
    }
    long code = 0;
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) != CURLE_OK) {
      return false;
    }
    if (code < 200 || code >= 300 || sink.length < 3) {
      return false;
    }
    if (sink.data[0] != 0xFF || sink.data[1] != 0xD8 || sink.data[2] != 0xFF) {
      return false;
    }
    if (!jpeg_decode(sink.data, sink.length, image)) {
      return false;
    }
  }
  const sys::MallocArray<unsigned char> decoded{image.rgb};
  constexpr std::size_t SCALED_BYTES = static_cast<std::size_t>(cfg::THUMB_IMG_W) *
                                       static_cast<std::size_t>(cfg::THUMB_IMG_H) * 3u;
  const sys::MallocArray<unsigned char> scaled{
      static_cast<unsigned char *>(std::malloc(SCALED_BYTES))};
  if (!scaled) {
    return false;
  }
  crop_resize(image, scaled.get());
  if (!from_disk) {
    unsigned char *encoded = nullptr;
    unsigned long encoded_length = 0;
    if (jpeg_encode(scaled.get(), &encoded, &encoded_length)) {
      const sys::MallocArray<unsigned char> owned{encoded};
      (void)write_cached(path, encoded, static_cast<std::size_t>(encoded_length));
    }
  }
  swizzle_rgb565(scaled.get(), cfg::THUMB_IMG_W, cfg::THUMB_IMG_H, destination);
  GSPGPU_FlushDataCache(destination, cfg::THUMB_TEX_BYTES);
  return true;
}

}  // namespace thumbs
}  // namespace mm
