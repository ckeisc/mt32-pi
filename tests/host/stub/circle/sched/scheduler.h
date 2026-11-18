// Host stub for <circle/sched/scheduler.h>: Yield maps to a thread yield.
// Only used by MU50's PCMTaskIteration idle/parked branches (M4,
// mirroring Nuked's iteration); the Pi build uses real Circle.
#ifndef ENGSTUB_CIRCLE_SCHED_SCHEDULER_H
#define ENGSTUB_CIRCLE_SCHED_SCHEDULER_H
#include <thread>
class CScheduler {
public:
  static CScheduler *Get() { static CScheduler s; return &s; }
  void Yield() { std::this_thread::yield(); }
};
#endif
