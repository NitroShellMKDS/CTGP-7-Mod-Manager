#include "backend/audio.h"

#include "backend/sd_card.h"
#include "core/config.h"
#include "core/status.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace mm {
namespace audio {

OggVorbis_File stream{};
bool loop_mode = false;
std::atomic<bool> should_stop{false};
long sample_rate = 32768;
int channel_count = 1;
bool ndsp_ready = false;
std::array<ndspWaveBuf, 2> wave_buffers{};
std::array<sys::LinearBuffer, 2> wave_memory;
sys::Thread worker;

std::size_t ogg_read(void *dest, std::size_t size, std::size_t count, void *source) {
  return std::fread(dest, size, count, static_cast<std::FILE *>(source));
}

int ogg_seek(void *source, ogg_int64_t offset, int whence) {
  if (offset < static_cast<ogg_int64_t>(std::numeric_limits<long>::min()) ||
      offset > static_cast<ogg_int64_t>(std::numeric_limits<long>::max())) {
    return -1;
  }
  return std::fseek(static_cast<std::FILE *>(source), static_cast<long>(offset), whence) != 0
             ? -1
             : 0;
}

int ogg_close(void *source) {
  return std::fclose(static_cast<std::FILE *>(source));
}

long ogg_tell(void *source) {
  return std::ftell(static_cast<std::FILE *>(source));
}

const ov_callbacks CALLBACKS{&ogg_read, &ogg_seek, &ogg_close, &ogg_tell};

bool load(const char *path) {
  ov_clear(&stream);
  sys::FileHandle file = sd::open(path, "rb");
  if (!file) {
    return false;
  }
  if (ov_open_callbacks(file.get(), &stream, nullptr, 0, CALLBACKS) < 0) {
    return false;
  }
  (void)file.release();
  return true;
}

void play(bool loop) {
  loop_mode = loop;
  vorbis_info *info = ov_info(&stream, -1);
  if (info == nullptr) {
    return;
  }
  channel_count = std::clamp(info->channels, 1, cfg::AUDIO_MAX_CHANNELS);
  sample_rate = info->rate;
  ndspChnReset(0);
  ndspChnSetFormat(0, channel_count == 1 ? NDSP_FORMAT_MONO_PCM16
                                         : NDSP_FORMAT_STEREO_PCM16);
  ndspChnSetRate(0, static_cast<float>(sample_rate));
  std::array<float, 12> mix{};
  mix[0] = mix[1] = static_cast<float>(cfg::AUDIO_VOL) / 255.0f;
  ndspChnSetMix(0, mix.data());
  ndspSetOutputMode(NDSP_OUTPUT_STEREO);
  ndspSetMasterVol(1.0f);
}

int decode_frames(std::span<s16> destination, bool &reached_end) {
  reached_end = false;
  const int bytes_capacity = static_cast<int>(destination.size_bytes());
  char *out = reinterpret_cast<char *>(destination.data());
  int bytes_done = 0;
  constexpr int MAX_CONSECUTIVE_ERRORS = 64;
  int consecutive_errors = 0;
  while (bytes_done < bytes_capacity) {
    int bitstream = 0;
    const long read = ov_read(&stream, out + bytes_done,
                              bytes_capacity - bytes_done, &bitstream);
    if (read == 0) {
      reached_end = true;
      break;
    }
    if (read < 0) {
      if (++consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
        reached_end = true;
        break;
      }
      continue;
    }
    consecutive_errors = 0;
    bytes_done += static_cast<int>(read);
  }
  return bytes_done / (channel_count * static_cast<int>(sizeof(s16)));
}

void thread_main(void *) {
  play(false);
  int current = 0;
  bool first_buffer = true;
  while (!should_stop.load(std::memory_order_relaxed)) {
    ndspWaveBuf &buffer = wave_buffers[static_cast<std::size_t>(current)];
    if (!first_buffer) {
      while (buffer.status != NDSP_WBUF_DONE &&
             !should_stop.load(std::memory_order_relaxed)) {
        svcSleepThread(1'000'000ULL);
      }
      if (should_stop.load(std::memory_order_relaxed)) {
        break;
      }
    }
    first_buffer = false;
    const std::size_t sample_count =
        static_cast<std::size_t>(cfg::AUDIO_BUF_SAMPLES) *
        static_cast<std::size_t>(channel_count);
    const std::span<s16> pcm{buffer.data_pcm16, sample_count};
    bool reached_end = false;
    const int frames = decode_frames(pcm, reached_end);
    const std::size_t decoded_samples =
        static_cast<std::size_t>(frames) * static_cast<std::size_t>(channel_count);
    if (decoded_samples < sample_count) {
      const std::span<s16> tail = pcm.subspan(decoded_samples);
      std::memset(tail.data(), 0, tail.size_bytes());
    }
    buffer.nsamples = cfg::AUDIO_BUF_SAMPLES;
    buffer.status = NDSP_WBUF_DONE;
    ndspChnWaveBufAdd(0, &buffer);
    current ^= 1;
    if (!reached_end) {
      continue;
    }
    if (loop_mode) {
      ov_raw_seek(&stream, 0);
      continue;
    }
    if (!load("romfs:/loop.ogg")) {
      status::print("Audio: loop.ogg missing");
      break;
    }
    play(true);
    first_buffer = true;
  }
}

void init() {
  status::print("Audio: starting...");
  if (R_FAILED(ndspInit())) {
    status::print("Audio: ndspInit failed");
    return;
  }
  ndsp_ready = true;
  if (!load("romfs:/intro.ogg")) {
    status::print("Audio: intro.ogg missing");
    return;
  }
  {
    int bitstream = 0;
    std::array<char, 4096> probe{};
    const long decoded = ov_read(&stream, probe.data(),
                                 static_cast<int>(probe.size()), &bitstream);
    if (decoded <= 0) {
      status::print("Audio: intro.ogg decode failed ({})", decoded);
      ov_clear(&stream);
      return;
    }
    ov_raw_seek(&stream, 0);
  }
  for (sys::LinearBuffer &block : wave_memory) {
    block = sys::LinearBuffer{cfg::AUDIO_BUF_BYTES};
    if (!block) {
      status::print("Audio: linearAlloc failed");
      for (sys::LinearBuffer &allocated : wave_memory) {
        allocated.reset();
      }
      return;
    }
  }
  for (std::size_t i = 0; i < wave_buffers.size(); ++i) {
    std::memset(wave_memory[i].get(), 0, cfg::AUDIO_BUF_BYTES);
    wave_buffers[i].data_pcm16 = wave_memory[i].as<s16>();
    wave_buffers[i].nsamples = cfg::AUDIO_BUF_SAMPLES;
    wave_buffers[i].looping = false;
    wave_buffers[i].status = NDSP_WBUF_DONE;
  }
  worker = sys::Thread::spawn(&thread_main, nullptr, 128 * 1024,
                              cfg::AUDIO_PRIORITY, cfg::ANY_CORE);
  if (!worker) {
    status::print("Audio: thread creation failed");
    return;
  }
  status::print("Audio: thread started");
}

void shutdown() {
  should_stop.store(true, std::memory_order_relaxed);
  worker.join();
  if (ndsp_ready) {
    ndspChnReset(0);
    ndspExit();
    ndsp_ready = false;
  }
  ov_clear(&stream);
  for (sys::LinearBuffer &block : wave_memory) {
    block.reset();
  }
}

}  // namespace audio
}  // namespace mm
