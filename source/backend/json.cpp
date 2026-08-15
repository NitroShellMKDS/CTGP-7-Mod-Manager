#include "backend/json.h"

#include "backend/sd_card.h"

namespace mm {
namespace js {

bool is(json_object *object, json_type type) noexcept {
  return object != nullptr && json_object_get_type(object) == type;
}

json_object *field(json_object *object, const char *key, json_type type) noexcept {
  if (object == nullptr) {
    return nullptr;
  }
  json_object *value = nullptr;
  if (!json_object_object_get_ex(object, key, &value)) {
    return nullptr;
  }
  return is(value, type) ? value : nullptr;
}

const char *string_field(json_object *object, const char *key) noexcept {
  json_object *value = field(object, key, json_type_string);
  return value != nullptr ? json_object_get_string(value) : nullptr;
}

std::string string_or_empty(json_object *object, const char *key) {
  const char *text = string_field(object, key);
  return text != nullptr ? std::string{text} : std::string{};
}

ArrayView array_field(json_object *object, const char *key) noexcept {
  return ArrayView{field(object, key, json_type_array)};
}

bool add_field(json_object *object, const char *key, json_object *value) noexcept {
  if (value == nullptr) {
    return false;
  }
  if (json_object_object_add(object, key, value) != 0) {
    json_object_put(value);
    return false;
  }
  return true;
}

sys::JsonRef read_file(const char *path) {
  const sd::PathGuard guard{sd::path_lock};
  return sys::JsonRef{json_object_from_file(path)};
}

bool write_file(const char *path, json_object *root) {
  const sd::PathGuard guard{sd::path_lock};
  return json_object_to_file_ext(path, root, JSON_C_TO_STRING_NOSLASHESCAPE) >= 0;
}

}  // namespace js
}  // namespace mm
