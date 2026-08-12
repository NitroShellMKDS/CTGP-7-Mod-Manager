#pragma once

#include "core/system.h"

#include <3ds.h>
#include <3ds/ndsp/ndsp.h>
#include <tremor/ivorbisfile.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <span>

namespace mm {
namespace audio {

extern OggVorbis_File stream;
extern bool loop_mode;
extern std::atomic<bool> should_stop;
extern long sample_rate;
extern int channel_count;
extern bool ndsp_ready;
extern std::array<ndspWaveBuf, 2> wave_buffers;
extern std::array<sys::LinearBuffer, 2> wave_memory;
extern sys::Thread worker;
extern const ov_callbacks CALLBACKS;

std::size_t ogg_read(void *dest, std::size_t size, std::size_t count, void *source);

int ogg_seek(void *source, ogg_int64_t offset, int whence);

int ogg_close(void *source);

long ogg_tell(void *source);

[[nodiscard]] bool load(const char *path);

void play(bool loop);

[[nodiscard]] int decode_frames(std::span<s16> destination, bool &reached_end);

void thread_main(void *);

void init();

void shutdown();

}  // namespace audio
}  // namespace mm
