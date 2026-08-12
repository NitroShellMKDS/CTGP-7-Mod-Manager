#include "frontend/bottom_screen.h"

#include "backend/install.h"
#include "core/config.h"
#include "core/format.h"
#include "core/text.h"

#include <algorithm>
#include <atomic>

namespace mm {
namespace draw {

float measure_imgui(const char *content) {
  return ImGui::CalcTextSize(content).x;
}

void text_centered(const char *content, const ImVec4 &color, float y) {
  ImGui::SetCursorPos(ImVec2((cfg::BOT_W - ImGui::CalcTextSize(content).x) * 0.5f, y));
  ImGui::TextColored(color, "%s", content);
}

void bottom_status(const std::string &content, bool failed) {
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
    text_centered(busy ? "[B] Cancel  [START] Exit" : "[START] Exit",
                  cfg::IM_AUTHOR, cfg::HINT2_Y);
  }
  ImGui::End();
}

}  // namespace draw
}  // namespace mm
