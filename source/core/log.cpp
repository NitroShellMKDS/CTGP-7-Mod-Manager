#include <cstring>
#include "log.h"
#include "config.h"
#include "system.h"

static mm::sys::FileHandle logfile;
static std::string last_text;

void log_event(const std::string &text) {
  if (!logfile) {
    logfile = mm::sys::FileHandle{std::fopen(mm::cfg::LOG_FILE.data(), "w")};
    if (!logfile) {
      return;
    }
  }
  if (text != last_text) {
    fprintf(logfile.get(), "%s\n", text.c_str());
    fflush(logfile.get());
    last_text = text;
  }
}