// Dear ImGui text rendering for the 3DS, backed by citro2d and the shared system font.
// See imgui_c2d_text.h for the rationale.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_c2d_text.h"
#include "imgui_internal.h"

#include <3ds.h>
#include <citro2d.h>

#include <string.h>

namespace imgui_c2d {
namespace {

/* -------------------------------------------------------------------------- */
/*  Per-frame text queue                                                      */
/* -------------------------------------------------------------------------- */

/// One laid-out line of text. `str_off` indexes into s_arena, which holds the
/// NUL-terminated, already-clipped slice handed to citro2d.
struct Line
{
	int   str_off;
	float x, y;
};

/// One ImFont::RenderText() call, covering s_lines[first_line .. first_line+line_count).
struct TextCmd
{
	int   first_line;
	int   line_count;
	float scale;
	ImU32 col;
};

ImVector<char>    s_arena;
ImVector<Line>    s_lines;
ImVector<TextCmd> s_cmds;

C2D_TextBuf   s_text_buf = NULL;
FINF_s*       s_finf     = NULL;
ImFontLoader  s_loader;

/// Codepoints whose metrics are preloaded into ImGui. The atlas runs in legacy
/// (non-dynamic) mode, so glyphs outside these ranges fall back to the width of '?'
/// in ImGui's layout while citro2d still draws them at their true width. Metrics-only
/// glyphs cost ~40 bytes each and no texture space, so widening this is cheap.
const ImWchar kGlyphRanges[] =
{
	0x0020, 0x024F, // Basic Latin, Latin-1 Supplement, Latin Extended-A/B
	0x0370, 0x04FF, // Greek and Cyrillic
	0x2000, 0x206F, // General Punctuation (includes U+2026, the ellipsis)
	0,
};

/// The system font is authored at FINF_s::lineFeed pixels per line, so an ImGui font
/// size of 16px maps to 16/30 = 0.5333 -- the scale the pre-ImGui UI drew text at.
inline float scale_for_size(float size)
{
	return size / (float)s_finf->lineFeed;
}

bool uses_system_font(ImFont* font)
{
	return s_text_buf != NULL && font != NULL && font->Sources.Size > 0 &&
	       font->Sources[0]->FontLoader == &s_loader;
}

/* -------------------------------------------------------------------------- */
/*  Font loader: metrics only, no pixels                                      */
/* -------------------------------------------------------------------------- */

bool font_src_init(ImFontAtlas*, ImFontConfig*)
{
	return true;
}

void font_src_destroy(ImFontAtlas*, ImFontConfig*)
{
}

bool font_src_contains_glyph(ImFontAtlas*, ImFontConfig*, ImWchar codepoint)
{
	return C2D_FontGlyphIndexFromCodePoint(NULL, codepoint) != (int)s_finf->alterCharIndex;
}

bool font_baked_init(ImFontAtlas*, ImFontConfig*, ImFontBaked* baked, void*)
{
	const TGLP_s* tglp = s_finf->tglp;
	const float   scale = scale_for_size(baked->Size);
	baked->Ascent  = tglp->baselinePos * scale;
	baked->Descent = -(float)(tglp->cellHeight - tglp->baselinePos) * scale;
	return true;
}

bool font_baked_load_glyph(ImFontAtlas*, ImFontConfig*, ImFontBaked* baked, void*,
                           ImWchar codepoint, ImFontGlyph* out_glyph, float* out_advance_x)
{
	const float scale = scale_for_size(baked->Size);

	fontGlyphPos_s gp;
	C2D_FontCalcGlyphPos(NULL, &gp, C2D_FontGlyphIndexFromCodePoint(NULL, codepoint),
	                     GLYPH_POS_CALC_VTXCOORD, scale, scale);

	if (out_advance_x != NULL)
	{
		*out_advance_x = gp.xAdvance;
		return true;
	}

	out_glyph->AdvanceX = gp.xAdvance;
	out_glyph->X0       = gp.vtxcoord.left;
	out_glyph->Y0       = gp.vtxcoord.top;
	out_glyph->X1       = gp.vtxcoord.right;
	out_glyph->Y1       = gp.vtxcoord.bottom;
	out_glyph->Visible  = false;                     // pixels come from citro2d, not the atlas
	out_glyph->PackId   = ImFontAtlasRectId_Invalid; // ...so nothing gets packed
	return true;
}

/* -------------------------------------------------------------------------- */
/*  Drawing                                                                   */
/* -------------------------------------------------------------------------- */

void draw_callback(const ImDrawList*, const ImDrawCmd* cmd)
{
	const TextCmd& tc = s_cmds[(int)(intptr_t)cmd->UserCallbackData];

	for (int i = 0; i < tc.line_count; i++)
	{
		const Line& line = s_lines[tc.first_line + i];

		// Reusing one buffer per line is safe: C2D_DrawText emits its vertices before
		// returning and doesn't reference the parsed text afterwards.
		C2D_TextBufClear(s_text_buf);

		C2D_Text text;
		if (!C2D_TextFontParseLine(&text, NULL, s_text_buf, &s_arena[line.str_off], 0))
			continue;

		C2D_TextOptimize(&text);
		C2D_DrawText(&text, C2D_WithColor, line.x, line.y, 0.0f, tc.scale, tc.scale, tc.col);
	}
}

/// Queues one line of text, clipped horizontally at character granularity. citro2d has
/// no per-draw clip rect, so a glyph straddling an edge is dropped rather than sliced.
void emit_line(ImFontBaked* baked, float advance_scale, const ImVec4& clip_rect,
               const char* begin, const char* end, float x, float y, float line_height)
{
	if (begin >= end)
		return;
	if (y + line_height <= clip_rect.y || y >= clip_rect.w)
		return;

	const int str_off = s_arena.Size;
	float     cx      = x;
	float     draw_x  = x;
	bool      started = false;

	const char* p = begin;
	while (p < end)
	{
		const char*  char_begin = p;
		unsigned int c          = (unsigned int)*p;
		if (c < 0x80)
			p += 1;
		else
			p += ImTextCharFromUtf8(&c, p, end);

		if (c == '\r')
			continue;
		if (cx >= clip_rect.z)
			break;

		const float advance = baked->GetCharAdvance((ImWchar)c) * advance_scale;
		if (!started)
		{
			if (cx + advance <= clip_rect.x)
			{
				cx += advance; // entirely left of the clip rect
				continue;
			}
			started = true;
			draw_x  = cx;
		}

		const int bytes = (int)(p - char_begin);
		s_arena.resize(s_arena.Size + bytes);
		memcpy(s_arena.Data + s_arena.Size - bytes, char_begin, (size_t)bytes);
		cx += advance;
	}

	if (s_arena.Size == str_off)
		return; // every character was clipped away
	s_arena.push_back('\0');

	Line line;
	line.str_off = str_off;
	line.x       = draw_x;
	line.y       = y;
	s_lines.push_back(line);
}

/// Queues the lines accumulated since `first_line` as one draw command, inserted into
/// the draw list in order so the text composites correctly against ImGui's own geometry.
bool flush_lines(ImDrawList* draw_list, int first_line, float size, ImU32 col)
{
	if (s_lines.Size == first_line)
		return true; // fully clipped, nothing to draw

	TextCmd cmd;
	cmd.first_line = first_line;
	cmd.line_count = s_lines.Size - first_line;
	cmd.scale      = scale_for_size(size);
	cmd.col        = col;

	const int cmd_index = s_cmds.Size;
	s_cmds.push_back(cmd);
	draw_list->AddCallback(&draw_callback, (void*)(intptr_t)cmd_index);
	return true;
}

/// Trim trailing blanks and step over the line break. Mirrors the same-named static
/// helper in imgui_draw.cpp so wrapped rendering lands where CalcTextSize() expects.
const char* next_line_start(const char* text, const char* text_end)
{
	while (text < text_end && ImCharIsBlankA(*text))
		text++;
	if (text < text_end && *text == '\n')
		text++;
	return text;
}

} // namespace

/* -------------------------------------------------------------------------- */
/*  Public interface                                                          */
/* -------------------------------------------------------------------------- */

bool init(float font_size_px)
{
	if (s_text_buf != NULL)
		return true;
	if (R_FAILED(fontEnsureMapped()))
		return false;

	s_finf = C2D_FontGetInfo(NULL);
	if (s_finf == NULL || s_finf->tglp == NULL || s_finf->lineFeed == 0)
		return false;

	s_loader.Name                     = "3DS system font (citro2d)";
	s_loader.FontSrcInit              = font_src_init;
	s_loader.FontSrcDestroy           = font_src_destroy;
	s_loader.FontSrcContainsGlyph     = font_src_contains_glyph;
	s_loader.FontBakedInit            = font_baked_init;
	s_loader.FontBakedLoadGlyph       = font_baked_load_glyph;

	ImFontConfig cfg;
	cfg.FontLoader          = &s_loader;
	cfg.FontDataOwnedByAtlas = true; // no font data at all; keeps AddFont() from duplicating it
	cfg.SizePixels          = font_size_px;
	cfg.PixelSnapH          = false; // rounding advances would desync layout from citro2d
	cfg.OversampleH         = 1;
	cfg.OversampleV         = 1;
	cfg.GlyphRanges         = kGlyphRanges;
	ImFormatString(cfg.Name, IM_ARRAYSIZE(cfg.Name), "3DS system font, %.0fpx", font_size_px);

	ImFont* font = ImGui::GetIO().Fonts->AddFont(&cfg);
	if (font == NULL)
		return false;

	// Pin the special characters. ImGui otherwise picks U+FFFD as the fallback, which
	// would size its sparse per-codepoint index arrays to the whole BMP, and it would
	// synthesise "..." by copying the '.' glyph's packed rect -- which we never pack.
	font->FallbackChar     = (ImWchar)'?';
	font->EllipsisChar     = (ImWchar)0x2026;
	font->EllipsisAutoBake = false;

	s_text_buf = C2D_TextBufNew(4096);
	return s_text_buf != NULL;
}

void shutdown()
{
	if (s_text_buf != NULL)
	{
		C2D_TextBufDelete(s_text_buf);
		s_text_buf = NULL;
	}
	s_arena.clear();
	s_lines.clear();
	s_cmds.clear();
	s_finf = NULL;
}

void end_frame()
{
	s_arena.resize(0);
	s_lines.resize(0);
	s_cmds.resize(0);
}

bool render_text(ImFont* font, ImDrawList* draw_list, float size, const ImVec2& pos, ImU32 col,
                 const ImVec4& clip_rect, const char* text_begin, const char* text_end,
                 float wrap_width)
{
	if (!uses_system_font(font))
		return false;

	if (text_end == NULL)
		text_end = text_begin + strlen(text_begin);

	// Align to be pixel perfect, matching ImFont::RenderText().
	const float x = IM_TRUNC(pos.x);
	float       y = IM_TRUNC(pos.y);
	if (y > clip_rect.w || text_begin == text_end)
		return true;

	const float  line_height   = size;
	ImFontBaked* baked         = font->GetFontBaked(size);
	const float  advance_scale = size / baked->Size;
	const bool   wrap_enabled  = (wrap_width > 0.0f);

	// Fast-forward to the first visible line.
	const char* s = text_begin;
	while (y + line_height < clip_rect.y && s < text_end)
	{
		const char* line_end = (const char*)memchr(s, '\n', (size_t)(text_end - s));
		if (wrap_enabled)
		{
			s = font->CalcWordWrapPosition(size, s, line_end ? line_end : text_end, wrap_width);
			s = next_line_start(s, text_end);
		}
		else
		{
			s = line_end ? line_end + 1 : text_end;
		}
		y += line_height;
	}
	if (s == text_end)
		return true;

	// Walk lines the way ImFont::RenderText() does, so wrapping and vertical culling
	// agree with what CalcTextSizeA() measured.
	const int   first_line    = s_lines.Size;
	const char* line_begin    = s;
	const char* word_wrap_eol = NULL;

	while (s < text_end)
	{
		if (wrap_enabled)
		{
			if (word_wrap_eol == NULL)
				word_wrap_eol = font->CalcWordWrapPosition(size, s, text_end, wrap_width);

			if (s >= word_wrap_eol)
			{
				emit_line(baked, advance_scale, clip_rect, line_begin, s, x, y, line_height);
				y += line_height;
				if (y > clip_rect.w)
					return flush_lines(draw_list, first_line, size, col);
				word_wrap_eol = NULL;
				s             = next_line_start(s, text_end);
				line_begin    = s;
				continue;
			}
		}

		const char*  prev_s = s;
		unsigned int c      = (unsigned int)*s;
		if (c < 0x80)
			s += 1;
		else
			s += ImTextCharFromUtf8(&c, s, text_end);

		if (c == '\n')
		{
			emit_line(baked, advance_scale, clip_rect, line_begin, prev_s, x, y, line_height);
			y += line_height;
			if (y > clip_rect.w)
				return flush_lines(draw_list, first_line, size, col);
			line_begin = s;
		}
	}

	emit_line(baked, advance_scale, clip_rect, line_begin, text_end, x, y, line_height);
	return flush_lines(draw_list, first_line, size, col);
}

bool render_char(ImFont* font, ImDrawList* draw_list, float size, const ImVec2& pos, ImU32 col,
                 ImWchar c, const ImVec4* cpu_fine_clip)
{
	if (!uses_system_font(font))
		return false;

	// ImFont::RenderChar() is handed the fine clip rect only; the draw list's own clip
	// rect still applies, exactly as ImDrawList::AddText() combines them.
	ImVec4 clip_rect = draw_list->_CmdHeader.ClipRect;
	if (cpu_fine_clip != NULL)
	{
		clip_rect.x = ImMax(clip_rect.x, cpu_fine_clip->x);
		clip_rect.y = ImMax(clip_rect.y, cpu_fine_clip->y);
		clip_rect.z = ImMin(clip_rect.z, cpu_fine_clip->z);
		clip_rect.w = ImMin(clip_rect.w, cpu_fine_clip->w);
	}

	char utf8[5];
	ImTextCharToUtf8(utf8, (unsigned int)c);

	ImFontBaked* baked      = font->GetFontBaked(size);
	const int    first_line = s_lines.Size;
	emit_line(baked, size / baked->Size, clip_rect, utf8, utf8 + strlen(utf8),
	          IM_TRUNC(pos.x), IM_TRUNC(pos.y), size);
	return flush_lines(draw_list, first_line, size, col);
}

} // namespace imgui_c2d
