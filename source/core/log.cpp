#include <cstring>
#include "log.h"
#include "config.h"

static FILE *logfile = NULL;
static std::string last_text;

void log_event(const std::string &text) {
  if (!logfile) {
    logfile = fopen(mm::cfg::LOG_FILE.data(), "w");
  }
  if (logfile && text != last_text) {
    fprintf(logfile, "%s\n", text.c_str());
    fflush(logfile);
    last_text = text;
  }
}