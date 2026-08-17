#pragma once

#include "backend/store.h"
#include "core/config.h"
#include "frontend/model.h"

#include <3ds.h>
#include <citro2d.h>

#include <array>

namespace mm {
namespace draw {

struct CardText {
  C2D_Text name{};
  C2D_Text author{};
  C2D_Text updated{};
  C2D_Text status{};
  bool has_name = false;
  bool has_author = false;
  bool has_updated = false;
  bool has_status = false;
  float name_width = 0.0f;
  float author_width = 0.0f;
  float updated_width = 0.0f;
  float status_width = 0.0f;
};

extern C2D_TextBuf card_buffer;
extern C2D_TextBuf scratch_buffer;
extern std::array<CardText, cfg::CARDS_PER_PAGE> card_text;
extern int card_text_count;
extern float font_line_height;
extern float name_scale;
extern float author_scale;
extern float updated_scale;
extern float status_scale;
extern float message_scale;

[[nodiscard]] float measure_c2d(const char *content, float scale);

[[nodiscard]] constexpr u32 status_color(model::Priority priority) noexcept {
  switch (priority) {
    case model::Priority::UPDATE_AVAILABLE:
      return cfg::CLR_AMBER;
    case model::Priority::INSTALLED:
      return cfg::CLR_GREEN;
    case model::Priority::NOT_INSTALLED:
      break;
  }
  return cfg::CLR_GOLD;
}

[[nodiscard]] constexpr u32 card_accent(const store::ModData &mod, bool queued) noexcept {
  if (queued) {
    return cfg::CLR_QUEUE;
  }
  return status_color(model::priority_of(mod));
}

[[nodiscard]] bool init();

void shutdown() noexcept;

void rebuild_card_text();

void draw_card(int slot, const store::ModData &mod, bool selected);

void draw_top_message(const char *message, u32 color);

void top_screen();

}  // namespace draw
}  // namespace mm
