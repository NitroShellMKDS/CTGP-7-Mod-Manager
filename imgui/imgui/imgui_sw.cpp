// By Emil Ernerfeldt 2018
// LICENSE:
//   This software is dual-licensed to the public domain and under the following
//   license: you are granted a perpetual, irrevocable license to copy, modify,
//   publish, and distribute this file as you see fit.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_sw.h"

#include <cmath>
#include <vector>

#include "imgui.h"

#include <memory>

namespace imgui_sw {
	namespace {

		struct Stats
		{
			int    uniform_triangle_pixels = 0;
			int    textured_triangle_pixels = 0;
			int    gradient_triangle_pixels = 0;
			double uniform_rectangle_pixels = 0;
			double textured_rectangle_pixels = 0;
			double gradient_rectangle_pixels = 0;
		};

		struct Texture
		{
			uint8_t* pixels; // 8-bit.
			int            width;
			int            height;
		};

		struct PaintTarget
		{
			int       width;
			int       height;
			ImVec2    scale; // Multiply ImGui (point) coordinates with this to get pixel coordinates.
		};

		// ----------------------------------------------------------------------------

		struct Barycentric
		{
			float w0, w1, w2;
		};

		Barycentric operator*(const float f, const Barycentric& va)
		{
			return { f * va.w0, f * va.w1, f * va.w2 };
		}

		void operator+=(Barycentric& a, const Barycentric& b)
		{
			a.w0 += b.w0;
			a.w1 += b.w1;
			a.w2 += b.w2;
		}

		Barycentric operator+(const Barycentric& a, const Barycentric& b)
		{
			return Barycentric{ a.w0 + b.w0, a.w1 + b.w1, a.w2 + b.w2 };
		}

		// ----------------------------------------------------------------------------
		// Copies of functions in ImGui, inlined for speed:
		static inline int ImLerp(int a, int b, float t) { return (int)(a + (b - a) * t); }

		static inline ImU32 alpha_blend_colors(ImU32 col_a, ImU32 col_b)
		{
			float t = ((col_b >> IM_COL32_A_SHIFT) & 0xFF) / 255.f;
			int r = ImLerp((int)(col_a >> IM_COL32_R_SHIFT) & 0xFF, (int)(col_b >> IM_COL32_R_SHIFT) & 0xFF, t);
			int g = ImLerp((int)(col_a >> IM_COL32_G_SHIFT) & 0xFF, (int)(col_b >> IM_COL32_G_SHIFT) & 0xFF, t);
			int b = ImLerp((int)(col_a >> IM_COL32_B_SHIFT) & 0xFF, (int)(col_b >> IM_COL32_B_SHIFT) & 0xFF, t);
			return IM_COL32(r, g, b, 0xFF);
		}

		ImVec4 color_convert_u32_to_float4(ImU32 in)
		{
			const float s = 1.0f / 255.0f;
			return ImVec4(
				((in >> IM_COL32_R_SHIFT) & 0xFF) * s,
				((in >> IM_COL32_G_SHIFT) & 0xFF) * s,
				((in >> IM_COL32_B_SHIFT) & 0xFF) * s,
				((in >> IM_COL32_A_SHIFT) & 0xFF) * s);
		}

		ImU32 color_convert_float4_to_u32(const ImVec4& in)
		{
			ImU32 out;
			out = uint32_t(in.x * 255.0f + 0.5f) << IM_COL32_R_SHIFT;
			out |= uint32_t(in.y * 255.0f + 0.5f) << IM_COL32_G_SHIFT;
			out |= uint32_t(in.z * 255.0f + 0.5f) << IM_COL32_B_SHIFT;
			out |= uint32_t(in.w * 255.0f + 0.5f) << IM_COL32_A_SHIFT;
			return out;
		}

		// ----------------------------------------------------------------------------

		float min3(float a, float b, float c)
		{
			if (a < b&& a < c) { return a; }
			return b < c ? b : c;
		}

		float max3(float a, float b, float c)
		{
			if (a > b && a > c) { return a; }
			return b > c ? b : c;
		}

		float barycentric(const ImVec2& a, const ImVec2& b, const ImVec2& point)
		{
			return (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x);
		}

		void paint_uniform_rectangle(
			const PaintTarget& target,
			const ImVec2& min_f,
			const ImVec2& max_f,
			const ImU32& color)
		{
			// Integer bounding box [min, max):
			int min_x_i = static_cast<int>(target.scale.x * min_f.x + 0.5f);
			int min_y_i = static_cast<int>(target.scale.y * min_f.y + 0.5f);
			int max_x_i = static_cast<int>(target.scale.x * max_f.x + 0.5f);
			int max_y_i = static_cast<int>(target.scale.y * max_f.y + 0.5f);

			// Clamp to render target:
			min_x_i = std::max(min_x_i, 0);
			min_y_i = std::max(min_y_i, 0);
			max_x_i = std::min(max_x_i, target.width);
			max_y_i = std::min(max_y_i, target.height);

			// Clamp bottom edge to top edge
			max_x_i = std::max(min_x_i, max_x_i);
			max_y_i = std::max(min_y_i, max_y_i);


			C2D_DrawRectSolid(min_x_i, min_y_i, 0.0f, max_x_i - min_x_i, max_y_i - min_y_i, color);
		}

		void paint_gradient_rectangle(
			const PaintTarget& target,
			const ImVec2& min_f,
			const ImVec2& max_f,
			const ImU32& col0,
			const ImU32& col1,
			const ImU32& col2,
			const ImU32& col3
			)
		{
			// Integer bounding box [min, max):
			int min_x_i = static_cast<int>(target.scale.x * min_f.x + 0.5f);
			int min_y_i = static_cast<int>(target.scale.y * min_f.y + 0.5f);
			int max_x_i = static_cast<int>(target.scale.x * max_f.x + 0.5f);
			int max_y_i = static_cast<int>(target.scale.y * max_f.y + 0.5f);

			// Clamp to render target:
			min_x_i = std::max(min_x_i, 0);
			min_y_i = std::max(min_y_i, 0);
			max_x_i = std::min(max_x_i, target.width);
			max_y_i = std::min(max_y_i, target.height);

			// Clamp bottom edge to top edge
			max_x_i = std::max(min_x_i, max_x_i);
			max_y_i = std::max(min_y_i, max_y_i);

			C2D_DrawRectangle(min_x_i, min_y_i, 0.0f, max_x_i - min_x_i, max_y_i - min_y_i, 
				col0, col1, col2, col3);
			
		}

		static int s_imageCount;
		void paint_uniform_texture (
			const PaintTarget& target,
			C3D_Tex* texture,
			const ImVec4& clip_rect,
			const ImDrawVert& v0,
			const ImDrawVert& v1,
			const ImDrawVert& v2,
			Stats* stats)
		{
			const ImVec2 p0 = ImVec2(target.scale.x * v0.pos.x, target.scale.y * v0.pos.y);
			const ImVec2 p1 = ImVec2(target.scale.x * v1.pos.x, target.scale.y * v1.pos.y);
			const ImVec2 p2 = ImVec2(target.scale.x * v2.pos.x, target.scale.y * v2.pos.y);

			const auto rect_area = barycentric(p0, p1, p2); // Can be negative
			if (rect_area == 0.0f) { return; }

			// Find bounding box:
			float min_x_f = min3(p0.x, p1.x, p2.x);
			float min_y_f = min3(p0.y, p1.y, p2.y);
			float max_x_f = max3(p0.x, p1.x, p2.x);
			float max_y_f = max3(p0.y, p1.y, p2.y);

			// Clamp to clip_rect:
			min_x_f = std::max(min_x_f, target.scale.x * clip_rect.x);
			min_y_f = std::max(min_y_f, target.scale.y * clip_rect.y);
			max_x_f = std::min(max_x_f, target.scale.x * clip_rect.z);
			max_y_f = std::min(max_y_f, target.scale.y * clip_rect.w);

			// Clamp bottom edge to top edge
			max_x_f = std::max(min_x_f, max_x_f);
			max_y_f = std::max(min_y_f, max_y_f);

			// Inclusive [min, max] integer bounding box:
			int min_x_i = static_cast<int>(min_x_f + 0.5f);
			int min_y_i = static_cast<int>(min_y_f + 0.5f);
			int max_x_i = static_cast<int>(max_x_f + 0.5f);
			int max_y_i = static_cast<int>(max_y_f + 0.5f);

			// Clamp to render target:
			min_x_i = std::max(min_x_i, 0);
			min_y_i = std::max(min_y_i, 0);
			max_x_i = std::min(max_x_i, target.width - 1);
			max_y_i = std::min(max_y_i, target.height - 1);

			// ------------------------------------------------------------------------
			// Set up interpolation of barycentric coordinates:

			const auto topleft = ImVec2(min_x_i + 0.5f * target.scale.x,
				min_y_i + 0.5f * target.scale.y);
			const auto dx = ImVec2(1, 0);
			const auto dy = ImVec2(0, 1);

			const auto w0_topleft = barycentric(p1, p2, topleft);
			const auto w1_topleft = barycentric(p2, p0, topleft);
			const auto w2_topleft = barycentric(p0, p1, topleft);

			const auto w0_dx = barycentric(p1, p2, topleft + dx) - w0_topleft;
			const auto w1_dx = barycentric(p2, p0, topleft + dx) - w1_topleft;
			const auto w2_dx = barycentric(p0, p1, topleft + dx) - w2_topleft;

			const auto w0_dy = barycentric(p1, p2, topleft + dy) - w0_topleft;
			const auto w1_dy = barycentric(p2, p0, topleft + dy) - w1_topleft;
			const auto w2_dy = barycentric(p0, p1, topleft + dy) - w2_topleft;

			const Barycentric bary_0{ 1, 0, 0 };
			const Barycentric bary_1{ 0, 1, 0 };
			const Barycentric bary_2{ 0, 0, 1 };

			const auto inv_area = 1 / rect_area;
			const Barycentric bary = inv_area * (w0_topleft * bary_0 + w1_topleft * bary_1 + w2_topleft * bary_2);

			//------------------------------------------------------
			// (0,0) in imgui is topleft
			// (0,1) in c2d is topleft
			const ImVec2 uv = v0.uv * bary.w0 + v1.uv * bary.w1 + v2.uv * bary.w2;
			uint16_t pixelWidth = max_x_i - min_x_i;
			uint16_t pixelHeight = max_y_i - min_y_i;

			float C2D_uv_left = uv.x - (0.5f / texture->width);
			float C2D_uv_top = 1.0f - uv.y + (0.5f / texture->height);
			float C2D_uv_right = C2D_uv_left + ((pixelWidth + 0.5f) / texture->width);
			float C2D_uv_bot = C2D_uv_top - ((pixelHeight + 0.5f) / texture->height);
			
			Tex3DS_SubTexture subt3x = { pixelWidth, pixelHeight, C2D_uv_left, C2D_uv_top, C2D_uv_right, C2D_uv_bot };
			C2D_Image image = (C2D_Image){ texture, &subt3x };

			const C2D_ImageTint tint = { {
				{v0.col, 1.0f},
				{v0.col, 1.0f},
				{v0.col, 1.0f},
				{v0.col, 1.0f}
			} };
			C2D_DrawImageAt(image, min_x_i, min_y_i, 0.0f, &tint, 1.0f, 1.0f);
			// C2D_DrawRectangle(min_x_i, min_y_i, 0.0f, max_x_i - min_x_i, max_y_i - min_y_i,  v0.col, v0.col, v0.col, v0.col);
			s_imageCount++;
		}

		void paint_uniform_triangle(
			const PaintTarget& target,
			const ImVec4& clip_rect,
			const ImDrawVert& v0,
			const ImDrawVert& v1,
			const ImDrawVert& v2,
			Stats* stats)
		{
			// Find bounding box:
			float min_x_f = min3(v0.pos.x, v1.pos.x, v2.pos.x);
			float min_y_f = min3(v0.pos.y, v1.pos.y, v2.pos.y);
			float max_x_f = max3(v0.pos.x, v1.pos.x, v2.pos.x);
			float max_y_f = max3(v0.pos.y, v1.pos.y, v2.pos.y);

			// Draw triangle when not clipped
			bool notClipped = 	min_x_f >= target.scale.x * clip_rect.x && 
								min_y_f >= target.scale.y * clip_rect.y && 
								max_x_f <= target.scale.x * clip_rect.z &&
								max_y_f <= target.scale.y * clip_rect.w;

			// Clamp to clip_rect:
			min_x_f = std::max(min_x_f, target.scale.x * clip_rect.x);
			min_y_f = std::max(min_y_f, target.scale.y * clip_rect.y);
			max_x_f = std::min(max_x_f, target.scale.x * clip_rect.z);
			max_y_f = std::min(max_y_f, target.scale.y * clip_rect.w);
			
			// Clamp bottom edge to top edge
			max_x_f = std::max(min_x_f, max_x_f);
			max_y_f = std::max(min_y_f, max_y_f);

			// Inclusive [min, max] integer bounding box:
			int min_x_i = static_cast<int>(min_x_f + 0.5f);
			int min_y_i = static_cast<int>(min_y_f + 0.5f);
			int max_x_i = static_cast<int>(max_x_f + 0.5f);
			int max_y_i = static_cast<int>(max_y_f + 0.5f);

			// Clamp to render target:
			min_x_i = std::max(min_x_i, 0);
			min_y_i = std::max(min_y_i, 0);
			max_x_i = std::min(max_x_i, target.width - 1);
			max_y_i = std::min(max_y_i, target.height - 1);

			if (notClipped) {
				C2D_DrawTriangle(v0.pos.x, v0.pos.y, v0.col,
					v1.pos.x, v1.pos.y, v1.col,
					v2.pos.x, v2.pos.y, v2.col, 0.0f
				);

			} else {
				// Precompute edge functions for barycentric test
				float x0 = v0.pos.x, y0 = v0.pos.y;
				float x1 = v1.pos.x, y1 = v1.pos.y;
				float x2 = v2.pos.x, y2 = v2.pos.y;

				float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
				if (area == 0.0f) return; // Degenerate triangle

				// Draw clipped triangle
				for (int y = min_y_i; y <= max_y_i; ++y) {
					int start_x = min_x_i;
					int end_x = start_x;
					
					// Draw by row
					for (int x = min_x_i; x <= max_x_i; ++x) {
						// Pixel center sampling
						float px = x + 0.5f;
						float py = y + 0.5f;
						
						// Compute barycentric coordinates
						float w0 = (x1 - px) * (y2 - py) - (x2 - px) * (y1 - py);
						float w1 = (x2 - px) * (y0 - py) - (x0 - px) * (y2 - py);
						float w2 = (x0 - px) * (y1 - py) - (x1 - px) * (y0 - py);
						
						// All weights must be same sign as area
						if ((w0 >= 0 && w1 >= 0 && w2 >= 0 && area > 0) ||
						(w0 <= 0 && w1 <= 0 && w2 <= 0 && area < 0))
						{
							// Inside triangle — draw pixel
							end_x += 1;
						} else {
							// Row ends. stop check.
							break;
						}
					}
					// Draw row
					C2D_DrawRectSolid((float)start_x, (float)y, 0.0f, end_x - start_x, 1.0f, v0.col);
				}

			}


		}

		void paint_draw_cmd(
			const PaintTarget& target,
			const ImDrawVert* vertices,
			const ImDrawIdx* idx_buffer,
			const ImDrawCmd& pcmd,
			const SwOptions& options,
			Stats* stats)
		{
			auto texture = reinterpret_cast<C3D_Tex*>(pcmd.GetTexID());
			assert(texture);

			// ImGui uses the first pixel for "white".
			const ImVec2 white_uv = ImGui::GetFontTexUvWhitePixel();
			// OLD: const ImVec2 white_uv = ImVec2(0.5f / texture->width, 0.5f / texture->height);

			for (unsigned int i = 0; i + 3 <= pcmd.ElemCount; ) {
				const ImDrawVert& v0 = vertices[idx_buffer[i + 0]];
				const ImDrawVert& v1 = vertices[idx_buffer[i + 1]];
				const ImDrawVert& v2 = vertices[idx_buffer[i + 2]];

				// A lot of the big stuff are uniformly colored rectangles,
				// so we can save a lot of CPU by detecting them:
				if (options.optimize_rectangles && i + 6 <= pcmd.ElemCount) {
					const ImDrawVert& v3 = vertices[idx_buffer[i + 3]];
					const ImDrawVert& v4 = vertices[idx_buffer[i + 4]];
					const ImDrawVert& v5 = vertices[idx_buffer[i + 5]];

					ImVec2 min, max;
					min.x = min3(v0.pos.x, v1.pos.x, v2.pos.x);
					min.y = min3(v0.pos.y, v1.pos.y, v2.pos.y);
					max.x = max3(v0.pos.x, v1.pos.x, v2.pos.x);
					max.y = max3(v0.pos.y, v1.pos.y, v2.pos.y);

					// Not the prettiest way to do this, but it catches all cases
					// of a rectangle split into two triangle.
					// TODO: Stop it from also assuming duplicate triangles is one rectangle.
					if ((v0.pos.x == min.x || v0.pos.x == max.x) &&
						(v0.pos.y == min.y || v0.pos.y == max.y) &&
						(v1.pos.x == min.x || v1.pos.x == max.x) &&
						(v1.pos.y == min.y || v1.pos.y == max.y) &&
						(v2.pos.x == min.x || v2.pos.x == max.x) &&
						(v2.pos.y == min.y || v2.pos.y == max.y) &&
						(v3.pos.x == min.x || v3.pos.x == max.x) &&
						(v3.pos.y == min.y || v3.pos.y == max.y) &&
						(v4.pos.x == min.x || v4.pos.x == max.x) &&
						(v4.pos.y == min.y || v4.pos.y == max.y) &&
						(v5.pos.x == min.x || v5.pos.x == max.x) &&
						(v5.pos.y == min.y || v5.pos.y == max.y))
					{
						const bool has_uniform_color =
							v0.col == v1.col &&
							v0.col == v2.col &&
							v0.col == v3.col &&
							v0.col == v4.col &&
							v0.col == v5.col;

						const bool has_texture =
							v0.uv != white_uv ||
							v1.uv != white_uv ||
							v2.uv != white_uv ||
							v3.uv != white_uv ||
							v4.uv != white_uv ||
							v5.uv != white_uv;

						min.x = std::max(min.x, pcmd.ClipRect.x);
						min.y = std::max(min.y, pcmd.ClipRect.y);
						max.x = std::min(max.x, pcmd.ClipRect.z);
						max.y = std::min(max.y, pcmd.ClipRect.w);

						const auto num_pixels = (max.x - min.x) * (max.y - min.y) * target.scale.x * target.scale.y;

						if (!has_texture && has_uniform_color) {
							paint_uniform_rectangle(target, min, max, v0.col);
							stats->uniform_rectangle_pixels += num_pixels;
							i += 6;
							continue;
						}
						else if (has_texture && has_uniform_color) {
							paint_uniform_texture(target, texture, pcmd.ClipRect, v0, v1, v2, stats);
							stats->textured_rectangle_pixels += num_pixels;
							i += 6;
							continue;
						}
						else if (!has_texture && !has_uniform_color) {
							paint_gradient_rectangle(target, min, max, v0.col, v1.col, v2.col, v3.col);
							stats->gradient_rectangle_pixels += num_pixels;
							i += 6;
							continue;
						}
					}
				}

				paint_uniform_triangle(target, pcmd.ClipRect, v0, v1, v2, stats);
				i += 3;
			}
		}

		void paint_draw_list(const PaintTarget& target, const ImDrawList* cmd_list, const SwOptions& options, Stats* stats)
		{
			const ImDrawIdx* idx_buffer = &cmd_list->IdxBuffer[0];
			const ImDrawVert* vertices = cmd_list->VtxBuffer.Data;

			for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.size(); cmd_i++)
			{
				const ImDrawCmd& pcmd = cmd_list->CmdBuffer[cmd_i];
				if (pcmd.UserCallback) {
					pcmd.UserCallback(cmd_list, &pcmd);
				}
				else {
					paint_draw_cmd(target, vertices, idx_buffer, pcmd, options, stats);
				}
				idx_buffer += pcmd.ElemCount;
			}
		}

	} // namespace

	void make_style_fast()
	{
		ImGuiStyle& style = ImGui::GetStyle();

		style.AntiAliasedLines = false;
		style.AntiAliasedFill = false;
		style.WindowRounding = 0;
		// style.Colors[ImGuiCol_WindowBg].w = 1.0; // Doesn't actually help much.
	}

	void bind_imgui_painting()
	{
		ImGuiIO& io = ImGui::GetIO();

		// Load default font (embedded in code):
		static C3D_Tex* tex = (C3D_Tex*)linearAlloc(sizeof(C3D_Tex));
		uint8_t* tex_data;
		int font_width, font_height;
		io.Fonts->GetTexDataAsAlpha8(&tex_data, &font_width, &font_height);

		C3D_TexInit(tex, font_width, font_height, GPU_A8);
		C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(tex, GPU_REPEAT, GPU_REPEAT);

		// Copy font texture to RAM
		for (u32 y = 0; y < tex->height; ++y)
		{
			for (u32 x = 0; x < tex->width; ++x)
			{
				uint32_t dest = ((((y >> 3) * (tex->width >> 3) + (x >> 3)) << 6) + ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)));
				uint8_t& pixel = ((uint8_t*)tex->data)[dest];
				pixel = tex_data[(y * tex->width) + x];
			}
		}

		io.Fonts->TexID = tex;
	}

	static Stats s_stats; // TODO: pass as an argument?

	void paint_imgui(int width_pixels, int height_pixels, const SwOptions& options)
	{
		const float width_points = ImGui::GetIO().DisplaySize.x;
		const float height_points = ImGui::GetIO().DisplaySize.y;
		const ImVec2 scale{ width_pixels / width_points, height_pixels / height_points };
		PaintTarget target{ width_pixels, height_pixels, scale };
		const ImDrawData* draw_data = ImGui::GetDrawData();
		s_imageCount = 0;
		s_stats = Stats{};
		for (int i = 0; i < draw_data->CmdListsCount; ++i) {
			paint_draw_list(target, draw_data->CmdLists[i], options, &s_stats);
		}
	}

	void unbind_imgui_painting()
	{
		ImGuiIO& io = ImGui::GetIO();
		delete reinterpret_cast<C3D_Tex*>( io.Fonts->TexData->TexID );
		io.Fonts = nullptr;
	}

	bool show_options(SwOptions* io_options)
	{
		assert(io_options);
		bool changed = false;
		changed |= ImGui::Checkbox("optimize_rectangles", &io_options->optimize_rectangles);
		return changed;
	}

	void show_stats()
	{
		ImGui::Text("uniform_triangle_pixels:   %7d", s_stats.uniform_triangle_pixels);
		ImGui::Text("textured_triangle_pixels:  %7d", s_stats.textured_triangle_pixels);
		ImGui::Text("gradient_triangle_pixels:  %7d", s_stats.gradient_triangle_pixels);
		ImGui::Text("uniform_rectangle_pixels:  %7.0f", s_stats.uniform_rectangle_pixels);
		ImGui::Text("textured_rectangle_pixels: %7.0f", s_stats.textured_rectangle_pixels);
		ImGui::Text("gradient_rectangle_pixels: %7.0f", s_stats.gradient_rectangle_pixels);
	}

} // namespace imgui_sw
