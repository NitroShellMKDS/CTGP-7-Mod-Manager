#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <curl/curl.h>

#include "imgui/imgui.h"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <string_view>

static_assert(CHAR_BIT == 8, "Byte-oriented buffer maths assumes 8-bit bytes.");
static_assert(sizeof(s16) == 2, "PCM16 framing assumes a 2-byte sample.");
static_assert(sizeof(u16) == 2, "RGB565 texel packing assumes a 2-byte texel.");

namespace mm {
namespace cfg {

inline constexpr std::string_view USER_AGENT = "CTGP-7-Mod-Manager/3.0";
inline constexpr std::string_view API_V10_INDEX = "https://gamebanana.com/apiv10/Mod/Index";
inline constexpr std::string_view API_CORE_DATA = "https://api.gamebanana.com/Core/Item/Data";
inline constexpr std::string_view CA_BUNDLE_PATH = "romfs:/cacert.pem";
inline constexpr std::string_view DOWNLOAD_BASE = "https://gamebanana.com/dl/";

inline constexpr std::size_t SOC_ALIGN = 0x1000;
inline constexpr std::size_t SOC_BUFFERSIZE = 0x100000;
inline constexpr std::size_t MAX_RESPONSE_SIZE = 512 * 1024;
inline constexpr int MAX_FETCH_ATTEMPTS = 4;
inline constexpr u64 RETRY_BASE_DELAY_NS = 500'000'000ULL;
inline constexpr u64 THROTTLE_DELAY_NS = 1'500'000'000ULL;
inline constexpr std::size_t FETCH_WORKERS = 6;
inline constexpr std::size_t WORKER_STACK_SIZE = 128 * 1024;
inline constexpr int WORKER_PRIORITY = 0x3F;
inline constexpr int AUDIO_PRIORITY = 0x30;
inline constexpr int ANY_CORE = -2;
inline constexpr int INDEX_PER_PAGE = 50;
inline constexpr int MAX_PAGES_PER_CAT = 200;
inline constexpr std::size_t CORE_BATCH_SIZE = 50;
inline constexpr std::size_t RESPONSE_RESERVE = 64 * 1024;
inline constexpr std::array<int, 18> CATEGORIES{35931, 10605, 35932, 35943, 35933, 35935,
                                                 35937, 35938, 35939, 35941, 35942, 35944,
                                                 35946, 35947, 35945, 35940, 35934, 35936};

inline constexpr std::string_view BASE_DIR = "sdmc:/";
inline constexpr std::string_view APP_DIR = "sdmc:/3ds/CTGP-7-Mod-Manager/";
inline constexpr std::string_view CACHE_DIR = "sdmc:/3ds/CTGP-7-Mod-Manager/cache/";
inline constexpr std::string_view LISTS_DIR = "sdmc:/3ds/CTGP-7-Mod-Manager/cache/lists/";
inline constexpr std::string_view THUMB_DIR = "sdmc:/3ds/CTGP-7-Mod-Manager/cache/images/";
inline constexpr std::string_view CTGP7_DIR = "sdmc:/CTGP-7/MyStuff/Characters/";
inline constexpr std::string_view LOG_FILE = "sdmc:/3ds/CTGP-7-Mod-Manager/output.log";
inline constexpr std::string_view MOD_LIST_FILE = "sdmc:/3ds/CTGP-7-Mod-Manager/cache/lists/modlist.json";
inline constexpr std::string_view BY_NAME_FILE = "sdmc:/3ds/CTGP-7-Mod-Manager/cache/lists/byname.json";
inline constexpr std::string_view BY_UPDATED_FILE = "sdmc:/3ds/CTGP-7-Mod-Manager/cache/lists/byupdated.json";
inline constexpr std::string_view INSTALLED_FILE = "sdmc:/3ds/CTGP-7-Mod-Manager/installed_mods.json";
inline constexpr std::string_view INSTALLED_TMP = "sdmc:/3ds/CTGP-7-Mod-Manager/installed_mods.json.tmp";
inline constexpr std::string_view DOWNLOAD_TMP = "sdmc:/3ds/CTGP-7-Mod-Manager/cache/download.tmp";

inline constexpr int AUDIO_BUF_SAMPLES = 4096;
inline constexpr int AUDIO_MAX_CHANNELS = 2;
inline constexpr int AUDIO_VOL = 0x50;
inline constexpr std::size_t AUDIO_BUF_BYTES = static_cast<std::size_t>(AUDIO_BUF_SAMPLES) *
                                               static_cast<std::size_t>(AUDIO_MAX_CHANNELS) *
                                               sizeof(s16);
static_assert(AUDIO_MAX_CHANNELS >= 2,
              "play() clamps to 2 channels; wave buffers must be sized for that.");

inline constexpr float TOP_W = 400.0f;
inline constexpr float TOP_H = 240.0f;
inline constexpr float BOT_W = 320.0f;
inline constexpr float BOT_H = 240.0f;
inline constexpr int GRID_COLS = 3;
inline constexpr int GRID_ROWS = 2;
inline constexpr int CARDS_PER_PAGE = GRID_COLS * GRID_ROWS;
inline constexpr float CELL_W = TOP_W / GRID_COLS;
inline constexpr float CELL_H = TOP_H / GRID_ROWS;
inline constexpr float CARD_MARGIN = 2.0f;
inline constexpr float CARD_BORDER = 2.0f;
inline constexpr float CARD_W = CELL_W - CARD_MARGIN * 2.0f;
inline constexpr float CARD_H = CELL_H - CARD_MARGIN * 2.0f;
inline constexpr float CONTENT_W = CARD_W - CARD_BORDER * 2.0f;
inline constexpr float CONTENT_H = CARD_H - CARD_BORDER * 2.0f;
inline constexpr float TEXT_MAX_W = CONTENT_W - 4.0f;
inline constexpr float THUMB_H = 70.0f;
inline constexpr float NAME_Y = 69.0f;
inline constexpr float NAME_PX = 14.0f;
inline constexpr float AUTHOR_Y = 80.0f;
inline constexpr float AUTHOR_PX = 11.0f;
inline constexpr float UPDATED_Y = 89.0f;
inline constexpr float UPDATED_PX = 11.0f;
inline constexpr float STATUS_Y = 98.0f;
inline constexpr float STATUS_PX = 14.0f;
inline constexpr float MSG_PX = 18.0f;
inline constexpr float BTN_W = 280.0f;
inline constexpr float BTN_X = (BOT_W - BTN_W) * 0.5f;
inline constexpr float ACTION_BTN_H = 44.0f;
inline constexpr float UNINST_BTN_H = 34.0f;
inline constexpr float SORT_LABEL_Y = 22.0f;
inline constexpr float SORT_ROW_Y = 42.0f;
inline constexpr float SEP_Y = 72.0f;
inline constexpr float ACTION_BTN_Y = 82.0f;
inline constexpr float UNINST_BTN_Y = 134.0f;
inline constexpr float COUNTER_Y = 170.0f;
inline constexpr float SEARCH_Y = 2.0f;
inline constexpr float HINT1_Y = 190.0f;
inline constexpr float HINT2_Y = 210.0f;
inline constexpr float PROG_BAR_Y = 128.0f;
inline constexpr float PROG_BAR_H = 4.0f;
inline constexpr float MSG_LINE_Y = 168.0f;
inline constexpr int MSG_MAX_LINES = 3;
inline constexpr float BTN_TEXT_PAD = 8.0f;
inline constexpr float BTN_ROUNDING = 5.0f;
inline constexpr float CORNER_BTN_W = 34.0f;
inline constexpr float CORNER_BTN_H = 32.0f;
inline constexpr float CORNER_BTN_X = 6.0f;
inline constexpr float CORNER_BTN_Y = 6.0f;
inline constexpr float CORNER_GAP = 4.0f;
inline constexpr float CLEAR_BTN_X = CORNER_BTN_X + CORNER_BTN_W + CORNER_GAP;
inline constexpr float CLEAR_BTN_Y = CORNER_BTN_Y;
inline constexpr int BTN_MAX_LINES = 2;
inline constexpr std::size_t CARD_TEXT_GLYPHS = 2048;
inline constexpr std::size_t SCRATCH_GLYPHS = 512;
inline constexpr std::size_t MEASURE_MAX_LEN = 192;
inline constexpr float NAV_REPEAT_DELAY = 0.35f;
inline constexpr float NAV_REPEAT_RATE = 0.07f;
inline constexpr int SEARCH_MAX_LEN = 64;
inline constexpr ImGuiWindowFlags SCREEN_WINDOW_FLAGS =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

inline constexpr const char *CHPACK_SUFFIX = ".chpack";
inline constexpr std::size_t INSTALL_NAME_MAX = 128;
inline constexpr std::size_t INSTALL_MAX_FILES = 64;
inline constexpr int INSTALL_MAX_ENTRIES = 20000;
inline constexpr int64_t INSTALL_MAX_FILE_BYTES = 64 * 1024 * 1024;
inline constexpr int64_t INSTALL_MAX_TOTAL_BYTES = 256 * 1024 * 1024;
inline constexpr std::size_t ARCHIVE_BLOCK_SIZE = 64 * 1024;
inline constexpr curl_off_t DOWNLOAD_MAX_BYTES = 64 * 1024 * 1024;
inline constexpr long DL_CONNECT_TIMEOUT = 20L;
inline constexpr long DL_LOW_SPEED_LIMIT = 1024L;
inline constexpr long DL_LOW_SPEED_TIME = 30L;
inline constexpr std::size_t INSTALL_STACK_SIZE = 256 * 1024;
inline constexpr s64 INSTALL_IDLE_WAIT_NS = 50'000'000LL;

inline constexpr int THUMB_IMG_W = 110;
inline constexpr int THUMB_IMG_H = 62;
inline constexpr int THUMB_JPEG_QUALITY = 50;
inline constexpr u16 THUMB_TEX_W = 128;
inline constexpr u16 THUMB_TEX_H = 64;
inline constexpr std::size_t THUMB_TEX_BYTES = static_cast<std::size_t>(THUMB_TEX_W) *
                                               static_cast<std::size_t>(THUMB_TEX_H) * 2u;
static_assert((THUMB_TEX_W & (THUMB_TEX_W - 1)) == 0, "PICA200 requires power-of-two textures.");
static_assert((THUMB_TEX_H & (THUMB_TEX_H - 1)) == 0, "PICA200 requires power-of-two textures.");
static_assert(THUMB_TEX_W >= THUMB_IMG_W && THUMB_TEX_H >= THUMB_IMG_H,
              "The texture must be able to hold the cached image.");

inline constexpr int THUMB_SLOTS = 16;
inline constexpr int THUMB_WORKERS = 3;
inline constexpr int THUMB_QUEUE = CARDS_PER_PAGE;
inline constexpr int THUMB_FAIL_RING = 64;
static_assert(THUMB_SLOTS > CARDS_PER_PAGE,
              "Eviction must always find a victim that is not currently on screen.");

inline constexpr std::size_t THUMB_URL_MAX = 192;
inline constexpr std::size_t THUMB_MAX_BYTES = 1024 * 1024;
inline constexpr unsigned THUMB_SRC_MAX_DIM = 4096;
inline constexpr std::size_t THUMB_SRC_MAX_PIXELS = 2'000'000;
inline constexpr s64 THUMB_IDLE_WAIT_NS = 50'000'000LL;

inline constexpr u32 CLR_BG = C2D_Color32(0x15, 0x1D, 0x23, 0xFF);
inline constexpr u32 CLR_SEL_BG = C2D_Color32(0x2A, 0x3B, 0x47, 0xFF);
inline constexpr u32 CLR_QUEUE = C2D_Color32(0x3B, 0x7A, 0xE0, 0xFF);
inline constexpr u32 CLR_QUEUE_SEL_BG = C2D_Color32(0x2D, 0x4E, 0x80, 0xFF);
inline constexpr u32 CLR_GOLD = C2D_Color32(0xAB, 0xA0, 0x22, 0xFF);
inline constexpr u32 CLR_GREEN = C2D_Color32(0x4C, 0xAF, 0x50, 0xFF);
inline constexpr u32 CLR_AMBER = C2D_Color32(0xFF, 0xC1, 0x07, 0xFF);
inline constexpr u32 CLR_AUTHOR = C2D_Color32(0x88, 0x88, 0x88, 0xFF);
inline constexpr u32 CLR_ERROR = C2D_Color32(0xFF, 0x55, 0x55, 0xFF);
inline constexpr u32 CLR_THUMB = C2D_Color32(0x00, 0x00, 0x00, 0xFF);
inline constexpr u32 CLR_SEP = CLR_SEL_BG;

[[nodiscard]] constexpr ImVec4 im_color(u8 r, u8 g, u8 b, u8 a = 0xFF) noexcept {
  return ImVec4(static_cast<float>(r) / 255.0f,
                static_cast<float>(g) / 255.0f,
                static_cast<float>(b) / 255.0f,
                static_cast<float>(a) / 255.0f);
}

inline constexpr ImVec4 IM_WINDOW_BG = im_color(0x15, 0x1D, 0x23);
inline constexpr ImVec4 IM_GOLD = im_color(0xAB, 0xA0, 0x22);
inline constexpr ImVec4 IM_AMBER = im_color(0xFF, 0xC1, 0x07);
inline constexpr ImVec4 IM_AUTHOR = im_color(0x88, 0x88, 0x88);
inline constexpr ImVec4 IM_ERROR = im_color(0xFF, 0x55, 0x55);
inline constexpr ImVec4 IM_BTN_BG = im_color(0x2A, 0x3B, 0x47);
inline constexpr ImVec4 IM_BTN_HOT = im_color(0x38, 0x4E, 0x5D);
inline constexpr ImVec4 IM_UNINST_BG = im_color(0x4A, 0x27, 0x27);
inline constexpr ImVec4 IM_UNINST_HOT = im_color(0x63, 0x34, 0x34);
inline constexpr ImVec4 IM_UNINST_FG = im_color(0xFF, 0x88, 0x88);

}  // namespace cfg
}  // namespace mm
