#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <citro3d.h>
#include <imgui.h>
#include <imgui_ctru.h>
#include <imgui_citro3d.h>

constexpr auto CLEAR_COLOR = 0x204B7AFF;
constexpr auto SCREEN_WIDTH = 400.0f;
constexpr auto SCREEN_HEIGHT = 480.0f;
constexpr auto FB_SCALE = 2.0f;
constexpr auto FB_WIDTH = SCREEN_WIDTH * FB_SCALE;
constexpr auto FB_HEIGHT = SCREEN_HEIGHT * FB_SCALE;
constexpr auto DISPLAY_TRANSFER_FLAGS =
		GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
		GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
		GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY);

extern "C" int gui() {
	gfxInitDefault();
	C3D_Init(2 * C3D_DEFAULT_CMDBUF_SIZE);
	C3D_RenderTarget* s_top = C3D_RenderTargetCreate(FB_HEIGHT * 0.5f, FB_WIDTH, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(s_top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	C3D_RenderTarget* s_bottom = C3D_RenderTargetCreate(FB_HEIGHT * 0.5f, FB_WIDTH * 0.8f, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(s_bottom, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	if (!imgui::ctru::init()) {
		printf("Failed to create GUI!\n");
		return 1;
	}
	imgui::citro3d::init();

	auto &io = ImGui::GetIO();
	io.IniFilename = nullptr; // disable imgui.ini file
	ImGui::StyleColorsDark();
	auto &style = ImGui::GetStyle();
	style.Colors[ImGuiCol_WindowBg].w = 0.8f;
	style.ScaleAllSizes(0.5f);
	io.DisplaySize = ImVec2(SCREEN_WIDTH, SCREEN_HEIGHT);
	io.DisplayFramebufferScale = ImVec2(FB_SCALE, FB_SCALE);

	return 0;
}