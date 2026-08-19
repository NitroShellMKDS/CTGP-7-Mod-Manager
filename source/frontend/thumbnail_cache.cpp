#include "frontend/thumbnail_cache.h"

#include "backend/sd_card.h"
#include "backend/store.h"
#include "backend/thumbnail_source.h"
#include "frontend/model.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace mm {
namespace thumbs {

Tex3DS_SubTexture sub_texture{};

void build_sub_texture() noexcept {
  const float scale_x = cfg::CONTENT_W / static_cast<float>(cfg::THUMB_IMG_W);
  const float scale_y = cfg::THUMB_H / static_cast<float>(cfg::THUMB_IMG_H);
  const float scale = std::max(scale_x, scale_y);
  const float visible_w =
      std::min(cfg::CONTENT_W / scale, static_cast<float>(cfg::THUMB_IMG_W));
  const float visible_h =
      std::min(cfg::THUMB_H / scale, static_cast<float>(cfg::THUMB_IMG_H));
  const float origin_x =
      std::max((static_cast<float>(cfg::THUMB_IMG_W) - visible_w) * 0.5f, 0.0f);
  const float origin_y =
      std::max((static_cast<float>(cfg::THUMB_IMG_H) - visible_h) * 0.5f, 0.0f);
  sub_texture.width = static_cast<u16>(cfg::CONTENT_W);
  sub_texture.height = static_cast<u16>(cfg::THUMB_H);
  sub_texture.left = origin_x / static_cast<float>(cfg::THUMB_TEX_W);
  sub_texture.right = (origin_x + visible_w) / static_cast<float>(cfg::THUMB_TEX_W);
  sub_texture.top = 1.0f - origin_y / static_cast<float>(cfg::THUMB_TEX_H);
  sub_texture.bottom = 1.0f - (origin_y + visible_h) / static_cast<float>(cfg::THUMB_TEX_H);
}

sys::Mutex lock;
sys::Event wake{true};
std::array<TextureSlot, cfg::THUMB_SLOTS> slots;
std::array<Publication, cfg::THUMB_WORKERS> publications;
std::array<sys::Thread, cfg::THUMB_WORKERS> workers;
std::array<WorkerContext, cfg::THUMB_WORKERS> worker_contexts;
std::array<Request, cfg::THUMB_QUEUE> queue;
int queue_head = 0;
int queue_size = 0;
std::array<int, cfg::THUMB_WORKERS> in_flight{};
std::array<int, cfg::THUMB_FAIL_RING> failures{};
int failure_cursor = 0;
int failure_count = 0;
std::array<int, cfg::CARDS_PER_PAGE> wanted{};
int wanted_count = 0;
u32 frame_counter = 0;
bool ready = false;

bool is_wanted(int mod_id) noexcept {
  return std::ranges::find(wanted.begin(), wanted.begin() + wanted_count, mod_id) !=
         wanted.begin() + wanted_count;
}

bool is_resident(int mod_id) noexcept {
  return std::ranges::any_of(slots, [mod_id](const TextureSlot &slot) {
    return slot.mod_id == mod_id;
  });
}

bool has_failed(int mod_id) noexcept {
  if (mod_id == 0) return true;
  for (int i = 0; i < failure_count; ++i) {
    if (failures[i] == mod_id) return true;
  }
  return false;
}

void mark_failed(int mod_id) noexcept {
  if (mod_id == 0 || has_failed(mod_id)) {
    return;
  }
  failures[static_cast<std::size_t>(failure_cursor)] = mod_id;
  failure_cursor = (failure_cursor + 1) % cfg::THUMB_FAIL_RING;
  if (failure_count < cfg::THUMB_FAIL_RING) {
    ++failure_count;
  }
}

bool is_in_flight(int mod_id) noexcept {
  return std::ranges::find(in_flight, mod_id) != in_flight.end();
}

TextureSlot *pick_slot() noexcept {
  for (TextureSlot &slot : slots) {
    if (slot.mod_id == 0) {
      return &slot;
    }
  }
  TextureSlot *best = nullptr;
  for (TextureSlot &slot : slots) {
    if (is_wanted(slot.mod_id)) {
      continue;
    }
    if (best == nullptr || slot.last_used < best->last_used) {
      best = &slot;
    }
  }
  return best;
}

void rebuild_wanted() noexcept {
  wanted_count = 0;
  const int visible = model::visible_count();
  for (int i = 0; i < visible && wanted_count < cfg::CARDS_PER_PAGE; ++i) {
    const store::ModData &mod =
        model::mods[static_cast<std::size_t>(model::window_start + i)];
    if (mod.id != 0 && !mod.thumbnail_url.empty()) {
      wanted[static_cast<std::size_t>(wanted_count++)] = mod.id;
    }
  }
}

void rebuild_queue() noexcept {
  queue_head = 0;
  queue_size = 0;
  const int visible = model::visible_count();
  for (int i = 0; i < visible && queue_size < cfg::THUMB_QUEUE; ++i) {
    const store::ModData &mod =
        model::mods[static_cast<std::size_t>(model::window_start + i)];
    if (mod.id == 0 || mod.thumbnail_url.empty()) {
      continue;
    }
    if (mod.thumbnail_url.size() >= cfg::THUMB_URL_MAX) {
      continue;
    }
    if (is_resident(mod.id) || has_failed(mod.id) || is_in_flight(mod.id)) {
      continue;
    }
    Request &request = queue[static_cast<std::size_t>(queue_size++)];
    request.mod_id = mod.id;
    request.url.fill('\0');
    std::ranges::copy(mod.thumbnail_url, request.url.begin());
  }
}

void worker_main(void *argument) {
  const int index = static_cast<WorkerContext *>(argument)->index;
  Publication &publication = publications[static_cast<std::size_t>(index)];
  sys::CurlHandle curl;
  if (curl) {
    configure_download(curl.get());
  }
  for (;;) {
    wake.clear();
    Request request;
    {
      const std::scoped_lock guard{lock};
      if (!quit_requested.load(std::memory_order_acquire) && queue_size > 0) {
        request = queue[queue_head];
        queue_head = (queue_head + 1) % cfg::THUMB_QUEUE;
        --queue_size;
        in_flight[static_cast<std::size_t>(index)] = request.mod_id;
      }
    }
    if (quit_requested.load(std::memory_order_acquire)) {
      break;
    }
    if (request.mod_id == 0) {
      wake.wait_for(cfg::THUMB_IDLE_WAIT_NS);
      continue;
    }
    const bool produced =
        produce(curl.get(), request.mod_id, request.url.data(),
                publication.pixels.as<u16>());
    if (quit_requested.load(std::memory_order_acquire)) {
      break;
    }
    publication.ack.clear();
    {
      const std::scoped_lock guard{lock};
      publication.mod_id = request.mod_id;
      publication.ok = produced;
      publication.ready = true;
    }
    for (;;) {
      publication.ack.wait_for(cfg::THUMB_IDLE_WAIT_NS);
      bool taken = false;
      {
        const std::scoped_lock guard{lock};
        taken = !publication.ready;
      }
      if (taken || quit_requested.load(std::memory_order_acquire)) {
        break;
      }
    }
    if (quit_requested.load(std::memory_order_acquire)) {
      break;
    }
  }
  const std::scoped_lock guard{lock};
  in_flight[static_cast<std::size_t>(index)] = 0;
}

void tick() {
  if (!ready) {
    return;
  }
  ++frame_counter;
  bool have_work = false;
  {
    const std::scoped_lock guard{lock};
    rebuild_wanted();
    for (std::size_t i = 0; i < publications.size(); ++i) {
      Publication &publication = publications[i];
      if (!publication.ready) {
        continue;
      }
      if (!publication.ok) {
        mark_failed(publication.mod_id);
      } else if (is_wanted(publication.mod_id) && !is_resident(publication.mod_id)) {
        if (TextureSlot *slot = pick_slot()) {
          publication.pixels.swap_with(slot->texture.data);
          slot->mod_id = publication.mod_id;
          slot->last_used = frame_counter;
        }
      }
      in_flight[i] = 0;
      publication.ready = false;
      publication.ack.signal();
    }
    rebuild_queue();
    have_work = queue_size > 0;
  }
  if (have_work) {
    wake.signal();
  }
}

C3D_Tex *texture_for(int mod_id) noexcept {
  if (!ready || mod_id == 0) {
    return nullptr;
  }
  for (TextureSlot &slot : slots) {
    if (slot.mod_id != mod_id) {
      continue;
    }
    slot.last_used = frame_counter;
    return &slot.texture;
  }
  return nullptr;
}

bool init() {
  if (ready) {
    return true;
  }
  (void)sd::make_directories(cfg::THUMB_DIR);
  build_sub_texture();
  std::size_t created = 0;
  for (; created < slots.size(); ++created) {
    TextureSlot &slot = slots[created];
    if (!C3D_TexInit(&slot.texture, cfg::THUMB_TEX_W, cfg::THUMB_TEX_H, GPU_RGB565)) {
      break;
    }
    C3D_TexSetFilter(&slot.texture, GPU_LINEAR, GPU_LINEAR);
    std::memset(slot.texture.data, 0, cfg::THUMB_TEX_BYTES);
    C3D_TexFlush(&slot.texture);
    slot.mod_id = 0;
    slot.last_used = 0;
  }
  std::size_t staged = 0;
  if (created == slots.size()) {
    for (; staged < publications.size(); ++staged) {
      Publication &publication = publications[staged];
      publication.pixels = sys::LinearBuffer{cfg::THUMB_TEX_BYTES};
      if (!publication.pixels) {
        break;
      }
      publication.mod_id = 0;
      publication.ready = false;
      publication.ok = false;
      in_flight[staged] = 0;
    }
  }
  if (created != slots.size() || staged != publications.size()) {
    for (std::size_t i = 0; i < created; ++i) {
      C3D_TexDelete(&slots[i].texture);
    }
    for (std::size_t i = 0; i < staged; ++i) {
      publications[i].pixels.reset();
    }
    for (TextureSlot &slot : slots) {
      slot.mod_id = 0;
    }
    return false;
  }
  quit_requested.store(false, std::memory_order_release);
  ready = true;
  for (std::size_t i = 0; i < workers.size(); ++i) {
    worker_contexts[i].index = static_cast<int>(i);
    workers[i] = sys::Thread::spawn(&worker_main, &worker_contexts[i],
                                    cfg::WORKER_STACK_SIZE, cfg::WORKER_PRIORITY,
                                    cfg::ANY_CORE);
  }
  return true;
}

void shutdown() {
  if (!ready) {
    return;
  }
  ready = false;
  quit_requested.store(true, std::memory_order_release);
  wake.signal();
  for (Publication &publication : publications) {
    publication.ack.signal();
  }
  for (sys::Thread &worker : workers) {
    worker.join();
  }
  for (Publication &publication : publications) {
    publication.pixels.reset();
  }
  for (TextureSlot &slot : slots) {
    C3D_TexDelete(&slot.texture);
    slot.mod_id = 0;
  }
}

}  // namespace thumbs
}  // namespace mm
