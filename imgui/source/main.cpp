#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <time.h>
#include <string.h>
#include "vshader_shbin.h"

#include "imgui/imgui.h"
#include "imgui/imgui_sw.h"

#define CLEAR_COLOR 0x68B0D8FF

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float x, y, z; } vertex;

static const vertex vertex_list[] =
{
	{ 200.0f, 200.0f, 0.5f },
	{ 100.0f, 40.0f, 0.5f },
	{ 300.0f, 40.0f, 0.5f },
};

#define vertex_list_count (sizeof(vertex_list)/sizeof(vertex_list[0]))

static DVLB_s* vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection;
static C3D_Mtx projection;

static void* vbo_data;

static void sceneInit(void)
{
	// Load the vertex shader, create a shader program and bind it
	vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
	C3D_BindProgram(&program);

	// Get the location of the uniforms
	uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");

	// Configure attributes for use with the vertex shader
	C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0=position
	AttrInfo_AddFixed(attrInfo, 1); // v1=color

	// Set the fixed attribute (color) to solid white
	C3D_FixedAttribSet(1, 1.0, 1.0, 1.0, 1.0);

	// Compute the projection matrix
	Mtx_OrthoTilt(&projection, 0.0, 400.0, 0.0, 240.0, 0.0, 1.0, true);

	// Create the VBO (vertex buffer object)
	vbo_data = linearAlloc(sizeof(vertex_list));
	memcpy(vbo_data, vertex_list, sizeof(vertex_list));

	// Configure buffers
	C3D_BufInfo* bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo_data, sizeof(vertex), 1, 0x0);

	// Configure the first fragment shading substage to just pass through the vertex color
	// See https://www.opengl.org/sdk/docs/man2/xhtml/glTexEnv.xml for more insight
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

static void sceneRender(void)
{
	// Update the uniforms
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);

	// Draw the VBO
	C3D_DrawArrays(GPU_TRIANGLES, 0, vertex_list_count);
}

static void sceneExit(void)
{
	// Free the VBO
	linearFree(vbo_data);

	// Free the shader program
	shaderProgramFree(&program);
	DVLB_Free(vshader_dvlb);
}

int main()
{
	uint16_t width = 320, height = 240;

	// Initialize graphics
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

	// Initialize the render target
	C3D_RenderTarget* target = C3D_RenderTargetCreate(height, width, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(target, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);

	// Initialize the scene
	sceneInit();

	ImGui::CreateContext();
	//ImGui::SetMouseCursor()
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)width, (float)height);
 	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.MouseDrawCursor = true;
	imgui_sw::bind_imgui_painting();
	imgui_sw::SwOptions sw_options;
	imgui_sw::make_style_fast();

	// Setup backend capabilities flags
	imgui_sw::ImGui_ImplC3D_Data* bd = IM_NEW(imgui_sw::ImGui_ImplC3D_Data)();
	io.BackendRendererUserData = (void*)bd;

	// Set render target and parameters
	bd->m_RenderTarget = target;
	bd->m_Width = width;
	bd->m_Height = height;

	TickCounter frameTime;
	touchPosition touch;
	// Main loop
	while (aptMainLoop())
	{
		hidScanInput();

		// Respond to user input
		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break; // break in order to return to hbmenu
			
		osTickCounterUpdate(&frameTime);
		io.DeltaTime = osTickCounterRead(&frameTime) * 0.001f;
		osTickCounterStart(&frameTime);

		ImGui::NewFrame();
		ImGui::ShowDemoWindow(NULL);
		ImGui::Render();

		hidTouchRead(&touch);

		//printf("px:%d, py:%d\n", touch.px, touch.py);
		if(touch.px && touch.py)
		{
			io.MouseDown[0] = true;
			io.MousePos = ImVec2(touch.px, touch.py);
		}
		else
		io.MouseDown[0] = false;

		// Render the scene
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(target);
			C2D_SceneTarget(target);
			sceneRender();

			C2D_Prepare();
			imgui_sw::paint_imgui(width, height, sw_options);
			C2D_Flush();
		C3D_FrameEnd(0);
	}

	// Deinitialize the scene
	sceneExit();

	// Deinitialize graphics
	C3D_Fini();
	gfxExit();
	return 0;
}
