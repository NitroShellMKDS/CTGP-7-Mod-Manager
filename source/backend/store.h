#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mm {
namespace store {

struct ModData {
  int id = 0;
  std::string name;
  std::string author;
  std::string thumbnail_url;
  std::string latest_file_url;
  int64_t latest_file_date = 0;
  std::string latest_file_name;
};

struct InstallRecord {
  int64_t date = 0;
  std::vector<std::string> files;
  std::string source_file_name;
};

class Registry {
 public:
  struct Entry {
    int id = 0;
    InstallRecord record;
  };

  [[nodiscard]] const InstallRecord *find(int id) const noexcept {
    const auto it = lower_bound(id);
    if (it == entries_.end() || it->id != id) {
      return nullptr;
    }
    return &it->record;
  }

  [[nodiscard]] bool contains(int id) const noexcept {
    return find(id) != nullptr;
  }

  void insert_or_assign(int id, InstallRecord record) {
    const auto it = lower_bound(id);
    if (it != entries_.end() && it->id == id) {
      it->record = std::move(record);
      return;
    }
    entries_.insert(it, Entry{id, std::move(record)});
  }

  bool erase(int id) {
    const auto it = lower_bound(id);
    if (it == entries_.end() || it->id != id) {
      return false;
    }
    entries_.erase(it);
    return true;
  }

  void clear() noexcept {
    entries_.clear();
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return entries_.size();
  }

  [[nodiscard]] const std::vector<Entry> &entries() const noexcept {
    return entries_;
  }

 private:
  [[nodiscard]] std::vector<Entry>::iterator lower_bound(int id) noexcept {
    return std::lower_bound(entries_.begin(), entries_.end(), id,
                            [](const Entry &entry, int key) {
                              return entry.id < key;
                            });
  }

  [[nodiscard]] std::vector<Entry>::const_iterator lower_bound(int id) const noexcept {
    return std::lower_bound(entries_.begin(), entries_.end(), id,
                            [](const Entry &entry, int key) {
                              return entry.id < key;
                            });
  }

  std::vector<Entry> entries_;
};

extern Registry installed;

[[nodiscard]] bool write_mod_list(const char *path, const std::vector<ModData> &mods);

[[nodiscard]] bool read_mod_list(const char *path, std::vector<ModData> &out);

[[nodiscard]] bool load_installed();

[[nodiscard]] bool save_installed();

}  // namespace store
}  // namespace mm
