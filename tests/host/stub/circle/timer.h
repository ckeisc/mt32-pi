// Host stub for <circle/timer.h>: steady-clock microseconds.
#ifndef ENGSTUB_CIRCLE_TIMER_H
#define ENGSTUB_CIRCLE_TIMER_H
#include <chrono>
#include <cstdint>
class CTimer {
public:
  static CTimer *Get() { static CTimer s; return &s; }
  std::uint64_t GetClockTicks() const {
    static const auto t0 = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count());
  }
};
#endif
