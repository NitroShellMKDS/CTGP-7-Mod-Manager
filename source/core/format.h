#pragma once

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace mm {
namespace fmt {

inline void format_placeholder_count_does_not_match_arguments() {}

template <typename... Args>
class Spec {
 public:
  template <std::size_t N>
  consteval Spec(const char (&literal)[N]) : text_{literal, N - 1} {
    std::size_t placeholders = 0;
    for (std::size_t i = 0; i + 1 < N - 1;) {
      const bool doubled = literal[i] == literal[i + 1];
      if ((literal[i] == '{' || literal[i] == '}') && doubled) {
        i += 2;
        continue;
      }
      if (literal[i] == '{' && literal[i + 1] == '}') {
        ++placeholders;
        i += 2;
        continue;
      }
      ++i;
    }
    if (placeholders != sizeof...(Args)) {
      format_placeholder_count_does_not_match_arguments();
    }
  }

  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return text_;
  }

 private:
  std::string_view text_;
};

inline void append(std::string &out, std::string_view value) {
  out.append(value);
}

inline void append(std::string &out, const std::string &value) {
  out.append(value);
}

inline void append(std::string &out, char value) {
  out.push_back(value);
}

inline void append(std::string &out, const char *value) {
  if (value != nullptr) {
    out.append(value);
  }
}

template <std::integral T>
void append(std::string &out, T value) {
  std::array<char, 24> digits{};
  const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
  out.append(digits.data(), result.ptr);
}

template <typename... Args>
[[nodiscard]] std::string format(Spec<std::type_identity_t<Args>...> spec, const Args &...args) {
  const std::string_view text = spec.view();
  std::string out;
  out.reserve(text.size() + sizeof...(Args) * 8);
  std::size_t cursor = 0;
  const auto copy_to_placeholder = [text, &out, &cursor] {
    while (cursor < text.size()) {
      const char current = text[cursor];
      const bool has_next = cursor + 1 < text.size();
      if ((current == '{' || current == '}') && has_next && text[cursor + 1] == current) {
        out.push_back(current);
        cursor += 2;
        continue;
      }
      if (current == '{' && has_next && text[cursor + 1] == '}') {
        cursor += 2;
        return true;
      }
      out.push_back(current);
      ++cursor;
    }
    return false;
  };
  (void)((copy_to_placeholder() ? (append(out, args), true) : false) && ...);
  (void)copy_to_placeholder();
  return out;
}

}  // namespace fmt
}  // namespace mm
