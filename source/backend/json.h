#pragma once

#include "core/system.h"

#include <json-c/json.h>

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace mm {
namespace js {

[[nodiscard]] bool is(json_object *object, json_type type) noexcept;

[[nodiscard]] json_object *field(json_object *object, const char *key, json_type type) noexcept;

template <std::integral T>
[[nodiscard]] std::optional<T> parse_integer(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  T value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] const char *string_field(json_object *object, const char *key) noexcept;

[[nodiscard]] std::string string_or_empty(json_object *object, const char *key);

template <std::integral T>
[[nodiscard]] T integer_field(json_object *object, const char *key) noexcept {
  json_object *value = nullptr;
  if (!json_object_object_get_ex(object, key, &value) || value == nullptr) {
    return T{};
  }
  switch (json_object_get_type(value)) {
    case json_type_int: {
      const int64_t raw = json_object_get_int64(value);
      if (raw < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
          raw > static_cast<int64_t>(std::numeric_limits<T>::max())) {
        return T{};
      }
      return static_cast<T>(raw);
    }
    case json_type_string: {
      const char *text = json_object_get_string(value);
      if (text == nullptr) {
        return T{};
      }
      return parse_integer<T>(std::string_view{text}).value_or(T{});
    }
    default:
      return T{};
  }
}

class ArrayView {
 public:
  class Iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = json_object *;

    Iterator() noexcept = default;

    Iterator(json_object *array, std::size_t index) noexcept : array_{array}, index_{index} {}

    [[nodiscard]] json_object *operator*() const noexcept {
      return json_object_array_get_idx(array_, index_);
    }

    Iterator &operator++() noexcept {
      ++index_;
      return *this;
    }

    Iterator operator++(int) noexcept {
      Iterator previous = *this;
      ++index_;
      return previous;
    }

    [[nodiscard]] bool operator==(const Iterator &) const noexcept = default;

   private:
    json_object *array_ = nullptr;
    std::size_t index_ = 0;
  };

  explicit ArrayView(json_object *array) noexcept
      : array_{is(array, json_type_array) ? array : nullptr},
        size_{array_ != nullptr ? json_object_array_length(array_) : 0} {}

  [[nodiscard]] Iterator begin() const noexcept {
    return Iterator{array_, 0};
  }

  [[nodiscard]] Iterator end() const noexcept {
    return Iterator{array_, size_};
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return size_;
  }

  [[nodiscard]] bool empty() const noexcept {
    return size_ == 0;
  }

 private:
  json_object *array_ = nullptr;
  std::size_t size_ = 0;
};

[[nodiscard]] ArrayView array_field(json_object *object, const char *key) noexcept;

[[nodiscard]] bool add_field(json_object *object, const char *key, json_object *value) noexcept;

[[nodiscard]] sys::JsonRef read_file(const char *path);

[[nodiscard]] bool write_file(const char *path, json_object *root);

}  // namespace js
}  // namespace mm
