#include "core/status.h"

namespace mm {
namespace status {

sys::Mutex lock;
std::string line = "Initializing...";
bool feed_done = false;

std::string current() {
  std::scoped_lock guard{lock};
  return line;
}

void mark_feed_done() {
  std::scoped_lock guard{lock};
  feed_done = true;
}

bool feed_finished() {
  std::scoped_lock guard{lock};
  return feed_done;
}

}  // namespace status
}  // namespace mm
