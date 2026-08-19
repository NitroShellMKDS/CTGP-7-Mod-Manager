#include "frontend/app.h"

#include "backend/audio.h"
#include "backend/feed.h"
#include "backend/install.h"
#include "backend/sd_card.h"
#include "backend/store.h"
#include "core/config.h"
#include "core/status.h"
#include "frontend/bottom_screen.h"
#include "frontend/model.h"
#include "frontend/thumbnail_cache.h"
#include "frontend/top_screen.h"

#include <citro2d.h>
#include <curl/curl.h>
#include <malloc.h>

#include <cfloat>

namespace mm {
namespace app {

void run_search_dialog() {
  SwkbdState swkbd;
  swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, cfg::SEARCH_MAX_LEN);
  swkbdSetValidation(&swkbd, SWKBD_ANYTHING, 0, 0);
  swkbdSetHintText(&swkbd, "Type to filter mods");
  if (!model::search_query.empty()) {
    swkbdSetInitialText(&swkbd, model::search_query.c_str());
  }
  swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
  swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "Search", true);
  char output[cfg::SEARCH_MAX_LEN * 4 + 1];
  if (swkbdInputText(&swkbd, output, sizeof(output)) == SWKBD_BUTTON_CONFIRM) {
    model::apply_search(output);
  }
}

bool enter_browse_state() {
  (void)store::load_installed();
  const char *preferred =
      model::sort_by_name
          ? (model::sort_reversed ? cfg::BY_NAME_REVERSED_FILE.data() : cfg::BY_NAME_FILE.data())
          : (model::sort_reversed ? cfg::BY_UPDATED_REVERSED_FILE.data()
                                  : cfg::BY_UPDATED_FILE.data());
  if (!store::read_mod_list(preferred, model::mods) || model::mods.empty()) {
    (void)store::read_mod_list(cfg::MOD_LIST_FILE.data(), model::mods);
  }
  if (model::mods.empty()) {
    return false;
  }
  model::window_start = 0;
  model::selected = 0;
  model::sort();
  model::all_mods = model::mods;
  model::search_query.clear();
  (void)thumbs::init();
  (void)install::init();
  return true;
}

void advance_state(bool feed_done) {
  if (model::state == model::AppState::FETCHING && feed_done) {
    model::state = model::AppState::LOADING;
    status::print("Loading mods...");
    return;
  }
  if (model::state != model::AppState::LOADING) {
    return;
  }
  if (enter_browse_state()) {
    model::state = model::AppState::BROWSING;
    return;
  }
  model::state = model::AppState::FAILED;
  model::error_text = "Failed to load mods.";
}

void handle_browse_input(u32 nav_keys, u32 pressed) {
  if (model::bottom_overlay == model::BottomOverlay::ABOUT ||
      install::is_uninstall_pending()) {
    if (install::is_uninstall_pending()) {
      if (pressed & KEY_A) {
        install::confirm_uninstall();
      }
      if (pressed & KEY_B) {
        install::cancel_uninstall_pending();
      }
    }
    return;
  }

  model::handle_nav(nav_keys);
  if (nav_keys != 0) {
    install::user_message.clear();
  }
  if (install::busy()) {
    if (pressed & KEY_A) {
      if (!install::is_uninstalling()) {
        (void)install::queue_selected_mod();
      }
    }
    if (pressed & KEY_B) {
      if (!install::is_uninstalling()) {
        install::cancel();
      }
    }
    if (pressed & KEY_X) {
      model::set_sort_mode(!model::sort_by_name);
    }
    if (pressed & KEY_Y) {
      if (!install::is_uninstalling()) {
        model::toggle_selected_queue();
      }
    }
    return;
  }
  if (pressed & KEY_A) {
    install::do_action();
  }
  if (pressed & KEY_B) {
    install::request_uninstall();
  }
  if (pressed & KEY_X) {
    model::set_sort_mode(!model::sort_by_name);
  }
  if (pressed & KEY_Y) {
    model::toggle_selected_queue();
  }
}

Platform::~Platform() {
  shutdown();
}

bool Platform::init() {
  gfxInitDefault();
  C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
  C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
  C2D_Prepare();
  graphics_ready_ = true;
  status::print("Starting up...");
  return init_screens() && init_romfs() && init_audio_and_network();
}

void Platform::run() {
  while (aptMainLoop()) {
    hidScanInput();
    const u32 pressed = hidKeysDown();
    const u32 held = hidKeysHeld();
    const u32 released = hidKeysUp();
    const float delta = tick_clock();
    update_pointer(held, released);
    const bool feed_done = status::feed_finished();
    advance_state(feed_done);
    const std::string status_line = status::current();
    if (feed_done && (pressed & KEY_START)) {
      break;
    }
    if (model::state == model::AppState::BROWSING) {
      install::tick();
      handle_browse_input(model::nav_repeat(pressed, held, delta), pressed);
    }
    render(status_line);
  }
}

bool Platform::init_screens() {
  top_ = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
  bottom_ = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
  if (top_ == nullptr || bottom_ == nullptr) {
    status::print("Fatal: failed to create render targets");
    return false;
  }
  ImGui::CreateContext();
  imgui_ready_ = true;
  io_ = &ImGui::GetIO();
  io_->DisplaySize = ImVec2(cfg::BOT_W, cfg::BOT_H);
  imgui_sw::bind_imgui_painting(16.0f);
  imgui_sw::make_style_fast();
  ImGuiStyle &style = ImGui::GetStyle();
  style.Colors[ImGuiCol_WindowBg] = cfg::IM_WINDOW_BG;
  style.WindowRounding = 0.0f;
  style.WindowPadding = ImVec2(0.0f, 0.0f);
  style.WindowBorderSize = 0.0f;
  style.DisabledAlpha = 0.35f;
  io_->DeltaTime = 1.0f / 60.0f;
  osTickCounterStart(&frame_timer_);
  if (!draw::init()) {
    status::print("Fatal: failed to set up text rendering");
    return false;
  }
  return true;
}

bool Platform::init_romfs() {
  if (R_FAILED(romfsInit())) {
    status::print("romfsInit failed - no CA bundle!");
    return false;
  }
  romfs_ready_ = true;
  if (!sd::exists(cfg::CA_BUNDLE_PATH.data())) {
    status::print("Fatal: CA bundle missing from ROMFS");
    return false;
  }
  return true;
}

bool Platform::init_audio_and_network() {
  audio::init();
  soc_buffer_.reset(static_cast<u32 *>(memalign(cfg::SOC_ALIGN, cfg::SOC_BUFFERSIZE)));
  if (!soc_buffer_ ||
      R_FAILED(socInit(soc_buffer_.get(), static_cast<u32>(cfg::SOC_BUFFERSIZE)))) {
    status::print("socInit failed!");
    return false;
  }
  soc_ready_ = true;
  if (R_FAILED(sslcInit(0))) {
    status::print("sslcInit failed!");
    return false;
  }
  sslc_ready_ = true;
  if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
    status::print("curl_global_init failed!");
    return false;
  }
  curl_ready_ = true;
  feed_thread_ = sys::Thread::spawn(&feed::thread_main, nullptr, 128 * 1024,
                                    cfg::WORKER_PRIORITY, cfg::ANY_CORE);
  if (!feed_thread_) {
    status::print("Fatal: failed to create fetch thread");
    return false;
  }
  return true;
}

float Platform::tick_clock() noexcept {
  osTickCounterUpdate(&frame_timer_);
  float delta = static_cast<float>(osTickCounterRead(&frame_timer_) * 0.001);
  osTickCounterStart(&frame_timer_);
  if (!(delta > 0.0f)) {
    delta = 1.0f / 60.0f;
  }
  io_->DeltaTime = delta;
  return delta;
}

void Platform::update_pointer(u32 held, u32 released) noexcept {
  touchPosition touch{};
  hidTouchRead(&touch);
  if (held & KEY_TOUCH) {
    io_->MouseDown[0] = true;
    io_->MousePos = ImVec2(static_cast<float>(touch.px), static_cast<float>(touch.py));
    return;
  }
  io_->MouseDown[0] = false;
  if (!(released & KEY_TOUCH)) {
    io_->MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
  }
}

void Platform::render(const std::string &status_line) {
  ImGui::NewFrame();
  if (model::state == model::AppState::BROWSING) {
    draw::bottom_browse();
  } else {
    const bool failed = model::state == model::AppState::FAILED;
    draw::bottom_status(failed ? model::error_text : status_line, failed);
  }
  ImGui::Render();
  C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
  thumbs::tick();
  C2D_TargetClear(top_, cfg::CLR_BG);
  C2D_SceneBegin(top_);
  C2D_Prepare();
  draw::top_screen();
  C2D_TargetClear(bottom_, cfg::CLR_BG);
  C2D_SceneBegin(bottom_);
  C2D_Prepare();
  C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                 GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                 GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
  imgui_sw::paint_imgui(static_cast<int>(cfg::BOT_W), static_cast<int>(cfg::BOT_H),
                        sw_options_);
  C2D_Flush();
  C3D_FrameEnd(0);
}

void Platform::shutdown() noexcept {
  feed::quit_requested.store(true, std::memory_order_release);
  feed_thread_.join();
  draw::shutdown();
  thumbs::shutdown();
  install::shutdown();
  if (curl_ready_) {
    curl_global_cleanup();
    curl_ready_ = false;
  }
  if (sslc_ready_) {
    sslcExit();
    sslc_ready_ = false;
  }
  if (soc_ready_) {
    socExit();
    soc_ready_ = false;
  }
  soc_buffer_.reset();
  audio::shutdown();
  if (romfs_ready_) {
    romfsExit();
    romfs_ready_ = false;
  }
  if (imgui_ready_) {
    imgui_sw::unbind_imgui_painting();
    ImGui::DestroyContext();
    imgui_ready_ = false;
  }
  if (graphics_ready_) {
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    graphics_ready_ = false;
  }
}

}  // namespace app
}  // namespace mm
