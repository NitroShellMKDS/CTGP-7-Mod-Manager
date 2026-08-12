#include "backend/store.h"

#include "backend/json.h"
#include "backend/sd_card.h"
#include "core/config.h"
#include "core/format.h"
#include "core/system.h"

#include <string_view>

namespace mm {
namespace store {

Registry installed;

bool write_mod_list(const char *path, const std::vector<ModData> &mods) {
  sys::JsonRef array{json_object_new_array()};
  if (!array) {
    return false;
  }
  for (const ModData &mod : mods) {
    sys::JsonRef entry{json_object_new_object()};
    if (!entry) {
      return false;
    }
    const bool built =
        js::add_field(entry.get(), "Id", json_object_new_int(mod.id)) &&
        js::add_field(entry.get(), "Name", json_object_new_string(mod.name.c_str())) &&
        js::add_field(entry.get(), "Author", json_object_new_string(mod.author.c_str())) &&
        js::add_field(entry.get(), "ThumbnailUrl",
                      json_object_new_string(mod.thumbnail_url.c_str())) &&
        js::add_field(entry.get(), "LatestFileUrl",
                      json_object_new_string(mod.latest_file_url.c_str())) &&
        js::add_field(entry.get(), "LatestFileDate",
                      json_object_new_int64(mod.latest_file_date)) &&
        js::add_field(entry.get(), "LatestFileName",
                      json_object_new_string(mod.latest_file_name.c_str()));
    if (!built) {
      return false;
    }
    if (json_object_array_add(array.get(), entry.get()) != 0) {
      return false;
    }
    (void)entry.release();
  }
  return js::write_file(path, array.get());
}

bool read_mod_list(const char *path, std::vector<ModData> &out) {
  out.clear();
  const sys::JsonRef root = js::read_file(path);
  if (!root || !js::is(root.get(), json_type_array)) {
    return false;
  }
  const js::ArrayView records{root.get()};
  out.reserve(records.size());
  for (json_object *record : records) {
    if (!js::is(record, json_type_object)) {
      continue;
    }
    ModData mod;
    mod.id = js::integer_field<int>(record, "Id");
    if (mod.id == 0) {
      continue;
    }
    mod.name = js::string_or_empty(record, "Name");
    mod.author = js::string_or_empty(record, "Author");
    mod.thumbnail_url = js::string_or_empty(record, "ThumbnailUrl");
    mod.latest_file_url = js::string_or_empty(record, "LatestFileUrl");
    mod.latest_file_name = js::string_or_empty(record, "LatestFileName");
    mod.latest_file_date = js::integer_field<int64_t>(record, "LatestFileDate");
    out.push_back(std::move(mod));
  }
  return true;
}

bool load_installed() {
  installed.clear();
  sys::JsonRef root = js::read_file(cfg::INSTALLED_FILE.c_str());
  if (!root) {
    root = js::read_file(cfg::INSTALLED_TMP.c_str());
  }
  if (!root) {
    return true;
  }
  if (!js::is(root.get(), json_type_object)) {
    return false;
  }
  json_object_object_foreach(root.get(), key, value) {
    if (key == nullptr || !js::is(value, json_type_object)) {
      continue;
    }
    const auto id = js::parse_integer<int>(std::string_view{key});
    if (!id || *id <= 0) {
      continue;
    }
    InstallRecord record;
    record.date = js::integer_field<int64_t>(value, "Date");
    record.source_file_name = js::string_or_empty(value, "SourceFileName");
    for (json_object *file : js::array_field(value, "Files")) {
      if (!js::is(file, json_type_string)) {
        continue;
      }
      const char *name = json_object_get_string(file);
      if (name != nullptr && name[0] != '\0') {
        record.files.emplace_back(name);
      }
    }
    installed.insert_or_assign(*id, std::move(record));
  }
  sd::unlink_quietly(cfg::INSTALLED_TMP.c_str());
  return true;
}

bool save_installed() {
  sys::JsonRef root{json_object_new_object()};
  if (!root) {
    return false;
  }
  for (const Registry::Entry &entry : installed.entries()) {
    sys::JsonRef record{json_object_new_object()};
    if (!record) {
      return false;
    }
    json_object *files = json_object_new_array();
    if (!js::add_field(record.get(), "Files", files)) {
      return false;
    }
    for (const std::string &file : entry.record.files) {
      json_object *item = json_object_new_string(file.c_str());
      if (item == nullptr) {
        return false;
      }
      if (json_object_array_add(files, item) != 0) {
        json_object_put(item);
        return false;
      }
    }
    if (!js::add_field(record.get(), "Date",
                       json_object_new_int64(entry.record.date)) ||
        !js::add_field(record.get(), "SourceFileName",
                       json_object_new_string(entry.record.source_file_name.c_str()))) {
      return false;
    }
    const std::string key = fmt::format("{}", entry.id);
    if (!js::add_field(root.get(), key.c_str(), record.get())) {
      return false;
    }
    (void)record.release();
  }
  if (!js::write_file(cfg::INSTALLED_TMP.c_str(), root.get())) {
    sd::unlink_quietly(cfg::INSTALLED_TMP.c_str());
    return false;
  }
  return sd::replace_file(cfg::INSTALLED_TMP.c_str(), cfg::INSTALLED_FILE.c_str());
}

}  // namespace store
}  // namespace mm
