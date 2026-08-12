#include "frontend/top_screen.h"

#include "core/text.h"
#include "frontend/thumbnail_cache.h"

#include <citro3d.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace mm {
namespace draw {

C2D_TextBuf card_buffer = nullptr;
C2D_TextBuf scratch_buffer = nullptr;
std::array<CardText, cfg::CARDS_PER_PAGE> card_text;
int card_text_count = 0;
float font_line_height = 30.0f;
float name_scale = 1.0f;
float author_scale = 1.0f;
float status_scale = 1.0f;
float message_scale = 1.0f;

float measure_c2d(const char *content, float scale) {
  if (scratch_buffer == nullptr || content == nullptr || content[0] == '\0') {
    return 0.0f;
  }
  C2D_TextBufClear(scratch_buffer);
  C2D_Text parsed;
  if (!C2D_TextFontParseLine(&parsed, nullptr, scratch_buffer, content, 0)) {
    return 0.0f;
  }
  return parsed.width * scale;
}

bool init() {
  if (R_FAILED(fontEnsureMapped())) {
    return false;
  }
  const FINF_s *info = C2D_FontGetInfo(nullptr);
  if (info != nullptr && info->lineFeed > 0) {
    font_line_height = static_cast<float>(info->lineFeed);
  }
  name_scale = cfg::NAME_PX / font_line_height;
  author_scale = cfg::AUTHOR_PX / font_line_height;
  status_scale = cfg::STATUS_PX / font_line_height;
  message_scale = cfg::MSG_PX / font_line_height;
  card_buffer = C2D_TextBufNew(cfg::CARD_TEXT_GLYPHS);
  scratch_buffer = C2D_TextBufNew(cfg::SCRATCH_GLYPHS);
  return card_buffer != nullptr && scratch_buffer != nullptr;
}

void shutdown() noexcept {
  if (card_buffer != nullptr) {
    C2D_TextBufDelete(card_buffer);
    card_buffer = nullptr;
  }
  if (scratch_buffer != nullptr) {
    C2D_TextBufDelete(scratch_buffer);
    scratch_buffer = nullptr;
  }
  card_text_count = 0;
}

void rebuild_card_text() {
  model::cards_dirty = false;
  card_text_count = 0;
  if (card_buffer == nullptr) {
    return;
  }
  C2D_TextBufClear(card_buffer);
  const auto parse = [](std::string_view source, float scale, C2D_Text &out,
                        float &out_width) -> bool {
    const std::string fitted = text::fit(
        source, cfg::TEXT_MAX_W, [scale](const char *candidate) {
          return measure_c2d(candidate, scale);
        });
    if (fitted.empty()) {
      return false;
    }
    if (!C2D_TextFontParseLine(&out, nullptr, card_buffer, fitted.c_str(), 0)) {
      return false;
    }
    C2D_TextOptimize(&out);
    out_width = out.width * scale;
    return true;
  };
  const int visible = model::visible_count();
  for (int i = 0; i < visible; ++i) {
    const store::ModData &mod =
        model::mods[static_cast<std::size_t>(model::window_start + i)];
    CardText &entry = card_text[static_cast<std::size_t>(i)];
    entry.has_name = parse(mod.name, name_scale, entry.name, entry.name_width);
    entry.has_author = parse(mod.author, author_scale, entry.author, entry.author_width);
    const model::Priority priority = model::priority_of(mod);
    entry.has_status =
        priority > model::Priority::NOT_INSTALLED &&
        parse(priority == model::Priority::UPDATE_AVAILABLE ? "Update Available"
                                                            : "Installed",
              status_scale, entry.status, entry.status_width);
  }
  card_text_count = visible;
}

void draw_card(int slot, const store::ModData &mod, bool selected) {
  const float x = static_cast<float>(slot % cfg::GRID_COLS) * cfg::CELL_W + cfg::CARD_MARGIN;
  const float y = static_cast<float>(slot / cfg::GRID_COLS) * cfg::CELL_H + cfg::CARD_MARGIN;
  const model::Priority priority = model::priority_of(mod);
  const u32 accent = status_color(priority);
  C2D_DrawRectSolid(x, y, 0.0f, cfg::CARD_W, cfg::CARD_H,
                    selected ? accent : cfg::CLR_BG);
  const float content_x = x + cfg::CARD_BORDER;
  const float content_y = y + cfg::CARD_BORDER;
  C2D_DrawRectSolid(content_x, content_y, 0.0f, cfg::CONTENT_W, cfg::CONTENT_H,
                    selected ? cfg::CLR_SEL_BG : cfg::CLR_BG);
  if (C3D_Tex *texture = thumbs::texture_for(mod.id)) {
    const C2D_Image image{texture, &thumbs::sub_texture};
    C2D_DrawImageAt(image, content_x, content_y, 0.0f, nullptr, 1.0f, 1.0f);
  } else {
    C2D_DrawRectSolid(content_x, content_y, 0.0f, cfg::CONTENT_W, cfg::THUMB_H,
                      cfg::CLR_THUMB);
  }
  if (slot >= card_text_count) {
    return;
  }
  const CardText &entry = card_text[static_cast<std::size_t>(slot)];
  if (entry.has_name) {
    C2D_DrawText(&entry.name, C2D_WithColor,
                 content_x + (cfg::CONTENT_W - entry.name_width) * 0.5f,
                 content_y + cfg::NAME_Y, 0.0f, name_scale, name_scale, accent);
  }
  if (entry.has_author) {
    C2D_DrawText(&entry.author, C2D_WithColor,
                 content_x + (cfg::CONTENT_W - entry.author_width) * 0.5f,
                 content_y + cfg::AUTHOR_Y, 0.0f, author_scale, author_scale,
                 cfg::CLR_AUTHOR);
  }
  if (entry.has_status) {
    C2D_DrawText(&entry.status, C2D_WithColor,
                 content_x + (cfg::CONTENT_W - entry.status_width) * 0.5f,
                 content_y + cfg::STATUS_Y, 0.0f, status_scale, status_scale,
                 accent);
  }
}

void draw_top_message(const char *message, u32 color) {
  if (scratch_buffer == nullptr || message == nullptr || message[0] == '\0') {
    return;
  }
  C2D_TextBufClear(scratch_buffer);
  C2D_Text parsed;
  if (!C2D_TextFontParseLine(&parsed, nullptr, scratch_buffer, message, 0)) {
    return;
  }
  C2D_TextOptimize(&parsed);
  C2D_DrawText(&parsed, C2D_WithColor,
               (cfg::TOP_W - parsed.width * message_scale) * 0.5f,
               (cfg::TOP_H - font_line_height * message_scale) * 0.5f,
               0.0f, message_scale, message_scale, color);
}

void top_screen() {
  if (model::state == model::AppState::FAILED) {
    draw_top_message(model::error_text.c_str(), cfg::CLR_ERROR);
    return;
  }
  if (model::state != model::AppState::BROWSING) {
    return;
  }
  if (model::cards_dirty) {
    rebuild_card_text();
  }
  const int visible = model::visible_count();
  if (visible <= 0) {
    draw_top_message("No mods found.", cfg::CLR_ERROR);
    return;
  }
  for (int i = 0; i < visible; ++i) {
    draw_card(i, model::mods[static_cast<std::size_t>(model::window_start + i)],
              i == model::selected);
  }
}

}  // namespace draw
}  // namespace mm
