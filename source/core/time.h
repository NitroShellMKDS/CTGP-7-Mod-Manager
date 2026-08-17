#pragma once

#include <cstdint>
#include <string>

namespace mm {
namespace timeutil {

// Format a mod file timestamp in UTC (GMT+0). If `ts_seconds` is 0, returns "Updated: unknown".
// Note: mod.latest_file_date is populated from the feed's _tsDateAdded field. If
// that metadata is absent, callers should fall back to file system last-modified
// time before calling this function. This fallback is documented here for clarity.
std::string format_update_label(int64_t ts_seconds);

}  // namespace timeutil
}  // namespace mm
