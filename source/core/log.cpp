#include <stdio.h>
#include "log.h"
#include "config.h" // const paths

FILE* logfile = NULL;

void log_event(const std::string &text) {
	if (!logfile) logfile = fopen(mm::cfg::LOG_FILE.c_str(), "w");
	fprintf(logfile, "%s\n", text.c_str());
}