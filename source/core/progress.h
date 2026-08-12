#pragma once

#include <atomic>
#include <cstddef>

namespace mm {
namespace progress {

extern std::atomic<int> jobs_done;
extern int jobs_total;
extern const char *phase_label;

void begin(const char *label, std::size_t total);

void step();

}  // namespace progress
}  // namespace mm
