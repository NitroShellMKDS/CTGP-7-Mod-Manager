#pragma once

#include "core/config.h"

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mm {
namespace text {

inline constexpr std::string_view ELLIPSIS = "\xE2\x80\xA6";

[[nodiscard]] constexpr std::size_t sequence_length(unsigned char lead) noexcept {
  if (lead < 0x80) {
    return 1;
  }
  if ((lead & 0xE0) == 0xC0) {
    return 2;
  }
  if ((lead & 0xF0) == 0xE0) {
    return 3;
  }
  if ((lead & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

[[nodiscard]] constexpr std::size_t step(std::string_view input, std::size_t index) noexcept {
  const std::size_t length = sequence_length(static_cast<unsigned char>(input[index]));
  return length <= input.size() - index ? length : 1;
}

void char_starts(std::string_view input, std::vector<std::size_t> &out);

[[nodiscard]] constexpr std::string_view clamp_bytes(std::string_view input,
                                                     std::size_t max_bytes) noexcept {
  if (input.size() <= max_bytes) {
    return input;
  }
  std::size_t cut = 0;
  while (cut < input.size()) {
    const std::size_t width = step(input, cut);
    if (cut + width > max_bytes) {
      break;
    }
    cut += width;
  }
  return input.substr(0, cut);
}

template <typename Fn>
concept WidthMeasure = std::invocable<Fn &, const char *> &&
                       std::convertible_to<std::invoke_result_t<Fn &, const char *>, float>;

template <WidthMeasure Fn>
[[nodiscard]] std::string fit(std::string_view input, float max_width, Fn measure) {
  if (input.empty() || max_width <= 0.0f) {
    return {};
  }
  const std::string source{clamp_bytes(input, cfg::MEASURE_MAX_LEN)};
  if (measure(source.c_str()) <= max_width) {
    return source;
  }
  std::vector<std::size_t> starts;
  char_starts(source, starts);
  std::size_t low = 0;
  std::size_t high = starts.size();
  std::size_t best = 0;
  while (low < high) {
    const std::size_t middle = low + (high - low) / 2;
    std::string candidate = source.substr(0, starts[middle]);
    candidate.append(ELLIPSIS);
    if (measure(candidate.c_str()) <= max_width) {
      best = middle;
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (best == 0) {
    return std::string{ELLIPSIS};
  }
  std::string result = source.substr(0, starts[best]);
  result.append(ELLIPSIS);
  return result;
}

}  // namespace text
}  // namespace mm
