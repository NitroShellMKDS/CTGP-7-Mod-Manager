#include "frontend/bottom_screen.h"

#include "backend/install.h"
#include "core/config.h"
#include "core/format.h"
#include "core/text.h"
#include "core/log.h"
#include "frontend/app.h"

#include <algorithm>
#include <atomic>

namespace mm {
namespace draw {

float measure_imgui(const char *content) {
  return ImGui::CalcTextSize(content).x;
}

void text_centered(const char *content, const ImVec4 &color, float y) {
  const float width = ImGui::CalcTextSize(content).x;
  ImGui::SetCursorPosX((cfg::BOT_W - width) * 0.5f);
  ImGui::SetCursorPosY(y);
  ImGui::TextColored(color, "%s", content);
}

void bottom_status(const std::string &content, bool failed) {
  log_event(content);
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(cfg::BOT_W, cfg::BOT_H));
  ImGui::Begin("##status", nullptr, cfg::SCREEN_WINDOW_FLAGS);
  const ImVec4 &color = failed ? cfg::IM_ERROR : cfg::IM_GOLD;
  const float wrap_width = cfg::BOT_W - 20.0f;
  const ImVec2 single = ImGui::CalcTextSize(content.c_str());
  if (single.x <= wrap_width) {
    text_centered(content.c_str(), color, (cfg::BOT_H - single.y) * 0.5f);
  } else {
    const ImVec2 wrapped = ImGui::CalcTextSize(content.c_str(), nullptr, false, wrap_width);
    ImGui::SetCursorPos(ImVec2(10.0f, (cfg::BOT_H - wrapped.y) * 0.5f));
    ImGui::PushTextWrapPos(10.0f + wrap_width);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", content.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
  }
  if (failed) {
    text_centered("Press START to exit.", cfg::IM_AUTHOR, cfg::BOT_H - 40.0f);
  }
  ImGui::End();
}

std::string action_label(model::ModAction action, const store::ModData *mod) {
  switch (action) {
    case model::ModAction::INSTALLED:
      return "Installed";
    case model::ModAction::NONE:
      return "No mods";
    case model::ModAction::INSTALL:
    case model::ModAction::UPDATE:
      break;
  }
  if (mod == nullptr) {
    return "No mods";
  }
  return fmt::format("{}{}", action == model::ModAction::UPDATE ? "Update " : "Install ",
                     mod->name);
}

void wrap_lines(std::string_view content, float wrap_width, int max_lines,
                std::vector<std::string> &out) {
  out.clear();
  if (content.empty() || wrap_width <= 0.0f || max_lines <= 0) {
    return;
  }
  ImFont *font = ImGui::GetFont();
  const float size = ImGui::GetFontSize();
  const char *cursor = content.data();
  const char *end = content.data() + content.size();
  while (cursor < end && static_cast<int>(out.size()) < max_lines) {
    const char *stop = font->CalcWordWrapPosition(size, cursor, end, wrap_width);
    if (stop <= cursor) {
      stop = cursor + 1;
    }
    if (static_cast<int>(out.size()) == max_lines - 1 && stop < end) {
      const std::string_view remainder{cursor, static_cast<std::size_t>(end - cursor)};
      out.push_back(text::fit(remainder, wrap_width, &measure_imgui));
      return;
    }
    out.emplace_back(cursor, stop);
    cursor = stop;
    while (cursor < end && *cursor == ' ') {
      ++cursor;
    }
  }
}

bool wrapped_button(const char *id, const std::string &label, float y,
                    float height, const ImVec4 &background,
                    const ImVec4 &background_hot, const ImVec4 &foreground,
                    float border) {
  ImGui::SetCursorPos(ImVec2(cfg::BTN_X, y));
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(cfg::BTN_W, height));
  const ImVec2 top_left = ImGui::GetItemRectMin();
  const ImVec2 bottom_right = ImGui::GetItemRectMax();
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  ImDrawList *list = ImGui::GetWindowDrawList();
  list->AddRectFilled(top_left, bottom_right,
                      ImGui::GetColorU32(hot ? background_hot : background),
                      cfg::BTN_ROUNDING);
  if (border > 0.0f) {
    list->AddRect(top_left, bottom_right, ImGui::GetColorU32(foreground),
                  cfg::BTN_ROUNDING, 0, border);
  }
  std::vector<std::string> lines;
  wrap_lines(label, cfg::BTN_W - cfg::BTN_TEXT_PAD * 2.0f, cfg::BTN_MAX_LINES, lines);
  const float line_height = ImGui::GetTextLineHeight();
  const ImU32 color = ImGui::GetColorU32(foreground);
  float text_y = top_left.y + (height - line_height * static_cast<float>(lines.size())) * 0.5f;
  for (const std::string &line : lines) {
    const float width = ImGui::CalcTextSize(line.c_str()).x;
    list->AddText(ImVec2(top_left.x + (cfg::BTN_W - width) * 0.5f, text_y), color,
                  line.c_str());
    text_y += line_height;
  }
  return pressed;
}

enum class CornerIcon { NONE, MAGNIFIER, X, QUESTION_MARK };

bool corner_button(const char *id, float x, float y, CornerIcon icon,
                   const ImVec4 &background, const ImVec4 &background_hot,
                   const ImVec4 &foreground) {
  const float w = cfg::CORNER_BTN_W;
  const float h = cfg::CORNER_BTN_H;
  ImGui::SetCursorPos(ImVec2(x, y));
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
  const ImVec2 top_left = ImGui::GetItemRectMin();
  const ImVec2 bottom_right = ImGui::GetItemRectMax();
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  ImDrawList *list = ImGui::GetWindowDrawList();
  const ImU32 background_color = ImGui::GetColorU32(hot ? background_hot : background);
  const ImU32 foreground_color = ImGui::GetColorU32(foreground);
  list->AddRectFilled(top_left, bottom_right, background_color, cfg::BTN_ROUNDING);
  list->AddRect(top_left, bottom_right, foreground_color, cfg::BTN_ROUNDING, 0, 1.0f);
  switch (icon) {
    case CornerIcon::MAGNIFIER: {
      const ImVec2 center(top_left.x + w * 0.42f, top_left.y + h * 0.46f);
      const float radius = w * 0.20f;
      list->AddCircle(center, radius, foreground_color, 16, 2.0f);
      const float inset = radius * 0.7071f * 0.85f;
      const float handle = w * 0.26f;
      const ImVec2 handle_start(center.x + inset, center.y + inset);
      list->AddLine(handle_start,
                    ImVec2(handle_start.x + handle, handle_start.y + handle),
                    foreground_color, 2.0f);
      break;
    }
    case CornerIcon::X: {
      const ImVec2 inner(top_left.x + w * 0.30f, top_left.y + h * 0.30f);
      const ImVec2 outer(top_left.x + w * 0.70f, top_left.y + h * 0.70f);
      list->AddLine(inner, outer, foreground_color, 2.5f);
      list->AddLine(ImVec2(outer.x, inner.y), ImVec2(inner.x, outer.y),
                    foreground_color, 2.5f);
      break;
    }
    case CornerIcon::NONE:
      break;
    case CornerIcon::QUESTION_MARK: {
      const float font_size = ImGui::GetFontSize();
      const ImVec2 text_size = ImGui::CalcTextSize("?");
      const ImVec2 text_pos(
          top_left.x + (w - text_size.x * 2.0f) * 0.5f,
          top_left.y + (h - text_size.y * 2.0f) * 0.5f);
      list->AddText(ImGui::GetFont(), font_size * 2.0f, text_pos, foreground_color, "?");
      break;
    }
  }
  return pressed;
}

void draw_sort_options() {
  text_centered("Sort Options", cfg::IM_GOLD, cfg::SORT_LABEL_Y);
  const ImGuiStyle &style = ImGui::GetStyle();
  const float radio = ImGui::GetFrameHeight();
  const float name_width = radio + style.ItemInnerSpacing.x + ImGui::CalcTextSize("By Name").x;
  const float updated_width =
      radio + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Recently Updated").x;
  ImGui::SetCursorPos(ImVec2(
      (cfg::BOT_W - (name_width + style.ItemSpacing.x + updated_width)) * 0.5f,
      cfg::SORT_ROW_Y));
  if (ImGui::RadioButton("By Name", model::sort_by_name)) {
    model::set_sort_mode(true);
  }
  ImGui::SameLine(0.0f, style.ItemSpacing.x);
  if (ImGui::RadioButton("Recently Updated", !model::sort_by_name)) {
    model::set_sort_mode(false);
  }
}

void draw_progress_bar() {
  ImDrawList *list = ImGui::GetWindowDrawList();
  list->AddRectFilled(ImVec2(cfg::BTN_X, cfg::PROG_BAR_Y),
                      ImVec2(cfg::BTN_X + cfg::BTN_W, cfg::PROG_BAR_Y + cfg::PROG_BAR_H),
                      cfg::CLR_SEP);
  const int done = install::percent.load(std::memory_order_relaxed);
  if (done <= 0) {
    return;
  }
  const float filled = cfg::BTN_W * static_cast<float>(std::min(done, 100)) / 100.0f;
  list->AddRectFilled(ImVec2(cfg::BTN_X, cfg::PROG_BAR_Y),
                      ImVec2(cfg::BTN_X + filled, cfg::PROG_BAR_Y + cfg::PROG_BAR_H),
                      ImGui::GetColorU32(cfg::IM_GOLD));
}

void bottom_browse() {
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(cfg::BOT_W, cfg::BOT_H));
  ImGui::Begin("##browse", nullptr, cfg::SCREEN_WINDOW_FLAGS);
  // If an overlay is active, draw it full-screen and ignore other controls.
  if (model::bottom_overlay == model::BottomOverlay::ABOUT) {
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    // Full replacement content for About/Credits
    static const char *about_content = "CTGP-7 Mod Manager: A 3DS homebrew app to manage CTGP-7 mods.\n\nCredits: NitroShell (Lead/Code), bonkmaykr (Code/Audio/Logo), MisakiP_ (Code/QA), Straky (Icon/QA), Orj_Osc (QA), gameonion (CTGP-7 Logo).\n\nProvided as-is. Use at your own risk.";
    // Title
    text_centered("About / Credits", cfg::IM_GOLD, 16.0f);
    ImGui::SetCursorPos(ImVec2(12.0f, 36.0f));
    ImGui::PushTextWrapPos(12.0f + (cfg::BOT_W - 24.0f));
    ImGui::TextWrapped(about_content);
    ImGui::PopTextWrapPos();
    // Large Close button centered at bottom
    const float close_y = cfg::BOT_H - (cfg::ACTION_BTN_H + 12.0f);
    ImGui::BeginDisabled(false);
    if (wrapped_button("##about_close", "Close", close_y, cfg::ACTION_BTN_H,
                       cfg::IM_BTN_BG, cfg::IM_BTN_HOT, cfg::IM_GOLD, 2.0f)) {
      model::bottom_overlay = model::BottomOverlay::NONE;
    }
    ImGui::EndDisabled();
    ImGui::End();
    return;
  }

  draw_sort_options();
  ImGui::GetWindowDrawList()->AddRectFilled(
      ImVec2(cfg::BTN_X, cfg::SEP_Y),
      ImVec2(cfg::BTN_X + cfg::BTN_W, cfg::SEP_Y + 2.0f),
      cfg::CLR_SEP);
  const bool busy = install::busy();
  {
    const model::ModAction action = model::current_action();
    const std::string label =
        busy ? install::progress_label() : action_label(action, model::selected_mod());
    const ImVec4 &foreground =
        action == model::ModAction::UPDATE ? cfg::IM_AMBER : cfg::IM_GOLD;
    ImGui::BeginDisabled(busy || (action != model::ModAction::INSTALL &&
                                  action != model::ModAction::UPDATE));
    if (wrapped_button("##action", label, cfg::ACTION_BTN_Y, cfg::ACTION_BTN_H,
                       cfg::IM_BTN_BG, cfg::IM_BTN_HOT, foreground, 2.0f)) {
      install::do_action();
    }
    ImGui::EndDisabled();
  }
  if (busy) {
    draw_progress_bar();
  }
  {
    const store::ModData *mod = model::selected_mod();
    const bool can_uninstall = !busy && mod != nullptr &&
                               store::installed.contains(mod->id);
    ImGui::BeginDisabled(!can_uninstall);
    if (wrapped_button("##uninstall", "Uninstall", cfg::UNINST_BTN_Y,
                       cfg::UNINST_BTN_H, cfg::IM_UNINST_BG, cfg::IM_UNINST_HOT,
                       cfg::IM_UNINST_FG, 1.0f)) {
      install::uninstall();
    }
    ImGui::EndDisabled();
  }
  {
    const int total = model::total_count();
    const int current =
        (total > 0 && model::selected_mod() != nullptr)
            ? model::window_start + model::selected + 1
            : 0;
    const std::string counter = fmt::format("{}/{}", current, total);
    text_centered(counter.c_str(), cfg::IM_AUTHOR, cfg::COUNTER_Y);
  }
  if (!install::user_message.empty()) {
    std::vector<std::string> lines;
    wrap_lines(install::user_message, cfg::BOT_W - 16.0f, cfg::MSG_MAX_LINES, lines);
    float y = cfg::MSG_LINE_Y;
    for (const std::string &line : lines) {
      text_centered(line.c_str(), cfg::IM_ERROR, y);
      y += ImGui::GetTextLineHeight() + 2.0f;
    }
  } else {
    text_centered("[A] Install  [B] Uninstall  [X] Sort", cfg::IM_AUTHOR, cfg::HINT1_Y);
    text_centered(busy ? "[B] Cancel  [START] Exit" : "[START] Exit", cfg::IM_AUTHOR,
                  cfg::HINT2_Y);
  }
  if (model::searching()) {
    text_centered(fmt::format("Filter: \"{}\"", model::search_query).c_str(),
                  cfg::IM_AMBER, cfg::SEARCH_Y);
  }

  const bool search_pressed =
      corner_button("##search", cfg::CORNER_BTN_X, cfg::CORNER_BTN_Y,
                    CornerIcon::MAGNIFIER, cfg::IM_BTN_BG, cfg::IM_BTN_HOT,
                    cfg::IM_GOLD);
  if (search_pressed) {
    app::run_search_dialog();
  }
  // About / Credits button in top-right corner of the bottom screen.
  const float about_x = cfg::BOT_W - cfg::CORNER_BTN_W - cfg::CORNER_BTN_X;
  const bool about_pressed = corner_button("##about", about_x, cfg::CORNER_BTN_Y,
                                          CornerIcon::QUESTION_MARK, cfg::IM_BTN_BG,
                                          cfg::IM_BTN_HOT, cfg::IM_GOLD);
  if (about_pressed) {
    model::bottom_overlay = model::BottomOverlay::ABOUT;
  }

  if (model::searching()) {
    const bool clear_pressed =
        corner_button("##clear", cfg::CLEAR_BTN_X, cfg::CLEAR_BTN_Y,
                      CornerIcon::X, cfg::IM_UNINST_BG, cfg::IM_UNINST_HOT,
                      cfg::IM_UNINST_FG);
    if (clear_pressed) {
      model::clear_search();
    }
  }
  ImGui::End();
}

}  // namespace draw
}  // namespace mm
