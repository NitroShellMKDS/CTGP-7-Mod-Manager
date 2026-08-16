#pragma once

#include "core/system.h"

#include <3ds.h>
#include <citro3d.h>

#include "imgui/imgui.h"
#include "imgui/imgui_sw.h"

#include <string>

namespace mm {
namespace app {

[[nodiscard]] bool enter_browse_state();

void run_search_dialog();

void advance_state(bool feed_done);

void handle_browse_input(u32 nav_keys, u32 pressed);

class Platform {
 public:
  Platform() = default;

  Platform(const Platform &) = delete;
  Platform &operator=(const Platform &) = delete;

  ~Platform();

  [[nodiscard]] bool init();

  void run();

 private:
  [[nodiscard]] bool init_screens();

  [[nodiscard]] bool init_romfs();

  [[nodiscard]] bool init_audio_and_network();

  [[nodiscard]] float tick_clock() noexcept;

  void update_pointer(u32 held, u32 released) noexcept;

  void render(const std::string &status_line);

  void shutdown() noexcept;

  C3D_RenderTarget *top_ = nullptr;
  C3D_RenderTarget *bottom_ = nullptr;
  ImGuiIO *io_ = nullptr;
  imgui_sw::SwOptions sw_options_{};
  TickCounter frame_timer_{};
  sys::MallocArray<u32> soc_buffer_;
  sys::Thread feed_thread_;
  bool graphics_ready_ = false;
  bool imgui_ready_ = false;
  bool romfs_ready_ = false;
  bool soc_ready_ = false;
  bool sslc_ready_ = false;
  bool curl_ready_ = false;
};

}  // namespace app
}  // namespace mm
