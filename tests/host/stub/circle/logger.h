// Host stub for <circle/logger.h>: LOG macros print to stderr.
#ifndef ENGSTUB_CIRCLE_LOGGER_H
#define ENGSTUB_CIRCLE_LOGGER_H
#include <cstdio>
#define LOGMODULE(name)
#define LOGNOTE(fmt, ...) std::fprintf(stderr, "NOTE: " fmt "\n", ##__VA_ARGS__)
#define LOGWARN(fmt, ...) std::fprintf(stderr, "WARN: " fmt "\n", ##__VA_ARGS__)
#define LOGERR(fmt, ...) std::fprintf(stderr, "ERR: " fmt "\n", ##__VA_ARGS__)
#define LOGDBG(fmt, ...) std::fprintf(stderr, "DBG: " fmt "\n", ##__VA_ARGS__)
#endif
