#include "core/time.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace mm {
namespace timeutil {

std::string format_update_label(int64_t ts_seconds) {
  if (ts_seconds <= 0) {
    return std::string("Updated: unknown");
  }
  std::time_t t = static_cast<std::time_t>(ts_seconds);
  std::tm *gmt = std::gmtime(&t);
  if (gmt == nullptr) return std::string("Updated: unknown");
  std::ostringstream out;
  out << std::put_time(gmt, "%b %d %Y");
  return std::string("Updated: ") + out.str();
}

}  // namespace timeutil
}  // namespace mm
