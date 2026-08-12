#include "core/progress.h"

#include "core/status.h"

namespace mm {
namespace progress {

std::atomic<int> jobs_done{0};
int jobs_total = 0;
const char *phase_label = "";

void begin(const char *label, std::size_t total) {
  phase_label = label;
  jobs_total = static_cast<int>(total);
  jobs_done.store(0, std::memory_order_relaxed);
  status::print("{}0/{}...", label, jobs_total);
}

void step() {
  const int done = jobs_done.fetch_add(1, std::memory_order_relaxed) + 1;
  status::print("{}{}/{}...", phase_label, done, jobs_total);
}

}  // namespace progress
}  // namespace mm
