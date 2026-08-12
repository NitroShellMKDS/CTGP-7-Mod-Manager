#include "frontend/model.h"

#include "core/config.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace mm {
namespace model {

AppState state = AppState::FETCHING;
std::vector<ModData> mods;
int window_start = 0;
int selected = 0;
bool sort_by_name = false;
std::string error_text;
bool cards_dirty = true;
u32 nav_held_key = 0;
float nav_timer = 0.0f;

Priority priority_of(const ModData &mod) noexcept {
  const store::InstallRecord *record = store::installed.find(mod.id);
  if (record == nullptr) {
    return Priority::NOT_INSTALLED;
  }
  return mod.latest_file_date > record->date ? Priority::UPDATE_AVAILABLE
                                             : Priority::INSTALLED;
}

void sort() {
  cards_dirty = true;
  const std::size_t count = mods.size();
  if (count <= 1) {
    return;
  }
  struct SortKey {
    Priority priority;
    u32 index;
  };
  std::vector<SortKey> keys(count);
  for (std::size_t i = 0; i < count; ++i) {
    keys[i] = SortKey{priority_of(mods[i]), static_cast<u32>(i)};
  }
  const bool by_name = sort_by_name;
  std::stable_sort(keys.begin(), keys.end(), [by_name](const SortKey &a, const SortKey &b) {
    if (a.priority != b.priority) {
      return a.priority > b.priority;
    }
    const ModData &left = mods[a.index];
    const ModData &right = mods[b.index];
    return by_name ? left.name < right.name : left.latest_file_date > right.latest_file_date;
  });
  std::vector<ModData> ordered;
  ordered.reserve(count);
  for (const SortKey &key : keys) {
    ordered.push_back(std::move(mods[key.index]));
  }
  mods = std::move(ordered);
}

int total_count() noexcept {
  return static_cast<int>(mods.size());
}

int visible_count() noexcept {
  const int remaining = total_count() - window_start;
  if (remaining <= 0) {
    return 0;
  }
  return std::min(remaining, cfg::CARDS_PER_PAGE);
}

int max_window_start() noexcept {
  const int total = total_count();
  if (total <= cfg::CARDS_PER_PAGE) {
    return 0;
  }
  const int rows = (total + cfg::GRID_COLS - 1) / cfg::GRID_COLS;
  const int start_row = rows - cfg::GRID_ROWS;
  return start_row > 0 ? start_row * cfg::GRID_COLS : 0;
}

const ModData *selected_mod() noexcept {
  const int visible = visible_count();
  if (visible <= 0 || selected < 0 || selected >= visible) {
    return nullptr;
  }
  return &mods[static_cast<std::size_t>(window_start + selected)];
}

ModAction current_action() noexcept {
  const ModData *mod = selected_mod();
  if (mod == nullptr) {
    return ModAction::NONE;
  }
  const store::InstallRecord *record = store::installed.find(mod->id);
  if (record == nullptr) {
    return ModAction::INSTALL;
  }
  return mod->latest_file_date > record->date ? ModAction::UPDATE : ModAction::INSTALLED;
}

void clamp_view() noexcept {
  window_start = std::clamp(window_start, 0, max_window_start());
  const int visible = visible_count();
  selected = visible > 0 ? std::clamp(selected, 0, visible - 1) : 0;
}

void resort_after_change() {
  sort();
  clamp_view();
  cards_dirty = true;
}

bool scroll(int rows) noexcept {
  const int before = window_start;
  window_start = std::clamp(window_start + rows * cfg::GRID_COLS, 0, max_window_start());
  if (window_start == before) {
    return false;
  }
  const int visible = visible_count();
  selected = visible > 0 ? std::clamp(selected, 0, visible - 1) : 0;
  cards_dirty = true;
  return true;
}

void handle_nav(u32 keys) noexcept {
  const int visible = visible_count();
  if (visible <= 0) {
    return;
  }
  int row_delta = 0;
  int col_delta = 0;
  if (keys & (KEY_DRIGHT | KEY_CPAD_RIGHT)) {
    col_delta = 1;
  } else if (keys & (KEY_DLEFT | KEY_CPAD_LEFT)) {
    col_delta = -1;
  } else if (keys & (KEY_DDOWN | KEY_CPAD_DOWN)) {
    row_delta = 1;
  } else if (keys & (KEY_DUP | KEY_CPAD_UP)) {
    row_delta = -1;
  } else {
    return;
  }
  const int row = selected / cfg::GRID_COLS;
  const int col = selected % cfg::GRID_COLS;
  if (col_delta != 0) {
    const int target_col = std::clamp(col + col_delta, 0, cfg::GRID_COLS - 1);
    selected = std::min(row * cfg::GRID_COLS + target_col, visible - 1);
    return;
  }
  const int last_row = (visible - 1) / cfg::GRID_COLS;
  const int target_row = row + row_delta;
  if (target_row >= 0 && target_row <= last_row) {
    selected = std::min(target_row * cfg::GRID_COLS + col, visible - 1);
    return;
  }
  (void)scroll(row_delta);
}

u32 nav_repeat(u32 pressed, u32 held, float delta_seconds) noexcept {
  constexpr u32 NAV_MASK = KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT |
                           KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT;
  if (const u32 fresh = pressed & NAV_MASK) {
    nav_held_key = fresh;
    nav_timer = cfg::NAV_REPEAT_DELAY;
    return fresh;
  }
  if (nav_held_key == 0 || (held & nav_held_key) == 0) {
    nav_held_key = 0;
    return 0;
  }
  nav_timer -= delta_seconds;
  if (nav_timer > 0.0f) {
    return 0;
  }
  nav_timer += cfg::NAV_REPEAT_RATE;
  if (nav_timer < 0.0f) {
    nav_timer = 0.0f;
  }
  return nav_held_key;
}

void set_sort_mode(bool by_name) {
  if (sort_by_name == by_name) {
    return;
  }
  sort_by_name = by_name;
  sort();
  window_start = 0;
  selected = 0;
  cards_dirty = true;
}

}  // namespace model
}  // namespace mm
