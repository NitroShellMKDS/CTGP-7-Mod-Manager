// Dear ImGui text rendering for the 3DS, backed by citro2d and the shared system font.
//
// ImGui normally rasterizes glyphs into its own font atlas and emits them as textured
// quads. On the 3DS that means a tiny aliased bitmap face blitted through the software
// painter. Instead we let citro2d draw the text with the system font (the same
// C2D_TextParse / C2D_DrawText path the pre-ImGui UI used), which is properly filtered
// and far more legible at 320x240.
//
// ImGui keeps owning layout: a custom ImFontLoader feeds it the system font's glyph
// advances and nothing else, so CalcTextSize(), button auto-sizing, alignment and word
// wrapping all stay in agreement with what citro2d actually draws.
//
// Enabled by IMGUI_3DS_C2D_TEXT (see imconfig.h), which makes ImFont::RenderText() and
// ImFont::RenderChar() defer to the hooks below.
#pragma once

#include "imgui.h"

namespace imgui_c2d {

/// Installs the 3DS system font as ImGui's font. Call after ImGui::CreateContext() and
/// before the font atlas is built. Returns false if the shared font is unavailable, in
/// which case the caller should fall back to ImFontAtlas::AddFontDefault().
bool init(float font_size_px);

/// Releases the citro2d text buffer. Safe to call if init() failed or never ran.
void shutdown();

/// Drops the text queued during the frame, keeping the storage for the next one.
/// Called at the end of imgui_sw::paint_imgui().
void end_frame();

// ---------------------------------------------------------------------------------
// Hooks called from imgui_draw.cpp. Both return false when the font isn't ours, which
// lets ImGui fall back to its own atlas-based rendering.
// ---------------------------------------------------------------------------------

bool render_text(ImFont* font, ImDrawList* draw_list, float size, const ImVec2& pos, ImU32 col,
                 const ImVec4& clip_rect, const char* text_begin, const char* text_end,
                 float wrap_width);

bool render_char(ImFont* font, ImDrawList* draw_list, float size, const ImVec2& pos, ImU32 col,
                 ImWchar c, const ImVec4* cpu_fine_clip);

} // namespace imgui_c2d
