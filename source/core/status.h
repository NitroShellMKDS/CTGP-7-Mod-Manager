#pragma once

#include "core/format.h"
#include "core/system.h"

#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

namespace mm {
namespace status {

extern sys::Mutex lock;
extern std::string line;
extern bool feed_done;

template <typename... Args>
void print(fmt::Spec<std::type_identity_t<Args>...> spec, const Args &...args) {
  std::string text = fmt::format(spec, args...);
  std::scoped_lock guard{lock};
  line = std::move(text);
}

[[nodiscard]] std::string current();

void mark_feed_done();

[[nodiscard]] bool feed_finished();

}  // namespace status
}  // namespace mm
