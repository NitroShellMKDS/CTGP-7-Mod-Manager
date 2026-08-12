#include "frontend/app.h"

int main() {
  mm::app::Platform platform;
  if (!platform.init()) {
    return 1;
  }
  platform.run();
  return 0;
}
