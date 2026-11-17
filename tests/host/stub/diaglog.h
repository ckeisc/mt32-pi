// Host stub for "diaglog.h": diag always enabled, interval 0 so a
// single UpdateLCD call flushes the whole window; LogLine output
// goes to the harness-set file (stderr if unset).
#ifndef ENGSTUB_DIAGLOG_H
#define ENGSTUB_DIAGLOG_H
#include <cstdarg>
#include <cstdio>
class CDiagLog {
public:
  static CDiagLog &Get() { static CDiagLog s; return s; }
  static void SetLogFile(FILE *f) { Get().m_pLog = f; }
  bool IsEnabled() const { return true; }
  unsigned IntervalSeconds() const { return 0; }
  void LogLine(const char *pFormat, ...) {
    FILE *f = m_pLog ? m_pLog : stderr;
    va_list ap; va_start(ap, pFormat);
    std::vfprintf(f, pFormat, ap);
    std::fprintf(f, "\n");
    va_end(ap);
  }
private:
  FILE *m_pLog = nullptr;
};
#endif
