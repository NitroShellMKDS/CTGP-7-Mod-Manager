#pragma once

#include "backend/store.h"

#include <3ds.h>

#include <string>
#include <vector>

namespace mm {
namespace model {

using store::ModData;

enum class AppState {
  FETCHING,
  LOADING,
  BROWSING,
  FAILED
};

enum class ModAction {
  NONE,
  INSTALL,
  UPDATE,
  INSTALLED
};

enum class Priority : int {
  NOT_INSTALLED = 1,
  INSTALLED = 2,
  UPDATE_AVAILABLE = 3
};

extern AppState state;
extern std::vector<ModData> mods;
extern std::vector<ModData> all_mods;
extern std::vector<int> queued_mod_ids;
extern std::string search_query;
extern int window_start;
extern int selected;
extern bool sort_by_name;
extern std::string error_text;
extern bool cards_dirty;
extern u32 nav_held_key;
extern float nav_timer;

enum class BottomOverlay { NONE = 0, ABOUT = 1 };

extern BottomOverlay bottom_overlay;

[[nodiscard]] Priority priority_of(const ModData &mod) noexcept;

void sort();

[[nodiscard]] int total_count() noexcept;

[[nodiscard]] int visible_count() noexcept;

[[nodiscard]] bool searching() noexcept;

void apply_search(const std::string &query);

void clear_search();

[[nodiscard]] int max_window_start() noexcept;

[[nodiscard]] const ModData *selected_mod() noexcept;

[[nodiscard]] bool queued(int mod_id) noexcept;

void queue_selected() noexcept;

void toggle_selected_queue() noexcept;

void remove_queued_mod(int mod_id) noexcept;

void clear_queued_mods() noexcept;

[[nodiscard]] ModAction current_action() noexcept;

void clamp_view() noexcept;

void resort_after_change();

bool scroll(int rows) noexcept;

void handle_nav(u32 keys) noexcept;

[[nodiscard]] u32 nav_repeat(u32 pressed, u32 held, float delta_seconds) noexcept;

void set_sort_mode(bool by_name);

}  // namespace model
}  // namespace mm
