#include "core/text.h"

namespace mm {
namespace text {

void char_starts(std::string_view input, std::vector<std::size_t> &out) {
  out.clear();
  for (std::size_t i = 0; i < input.size(); i += step(input, i)) {
    out.push_back(i);
  }
}

}  // namespace text
}  // namespace mm
