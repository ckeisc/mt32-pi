// Host stub for <circle/spinlock.h>: real std::mutex lock (M2: the
// split-mode engine shares CRingBuffer between the audio thread and
// the SWP thread, so a no-op lock would be a data race under
// ThreadSanitizer and in reality). Single-threaded harnesses are
// unaffected: sequential acquire/release on one thread is a no-op
// semantically, and the M1 eng_test PCM gate re-verifies that.
#ifndef ENGSTUB_CIRCLE_SPINLOCK_H
#define ENGSTUB_CIRCLE_SPINLOCK_H
#define TASK_LEVEL 0
#define IRQ_LEVEL 1
#include <mutex>
class CSpinLock {
public:
  explicit CSpinLock(int = TASK_LEVEL) {}
  void Acquire() { m_mutex.lock(); }
  void Release() { m_mutex.unlock(); }
private:
  std::mutex m_mutex;
};
#endif
