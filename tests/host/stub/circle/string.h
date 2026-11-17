// Host stub for <circle/string.h>: fixed-buffer CString. Deliberately
// NOT std::string-backed: the tree's Utility::QSort memcpy-swaps
// CString objects, which is only safe for a self-contained buffer.
#ifndef ENGSTUB_CIRCLE_STRING_H
#define ENGSTUB_CIRCLE_STRING_H
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <strings.h>
class CString {
public:
  CString() { m_Buffer[0] = 0; }
  CString(const char *p) { Set(p); }
  CString(const CString &o) { Set(static_cast<const char *>(o)); }
  CString &operator=(const CString &o) { if (this != &o) Set(static_cast<const char *>(o)); return *this; }
  CString &operator=(const char *p) { Set(p); return *this; }
  operator const char *() const { return m_Buffer; }
  void Format(const char *pFormat, ...) {
    va_list ap; va_start(ap, pFormat);
    std::vsnprintf(m_Buffer, sizeof m_Buffer, pFormat, ap);
    va_end(ap);
  }
private:
  void Set(const char *p) {
    if (!p) { m_Buffer[0] = 0; return; }
    std::strncpy(m_Buffer, p, sizeof m_Buffer - 1);
    m_Buffer[sizeof m_Buffer - 1] = 0;
  }
  char m_Buffer[512];
};
#endif
