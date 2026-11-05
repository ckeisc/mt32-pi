// mu50spsc.h -- single-producer/single-consumer queue for the MU50
// dual-core split (M2). Circle-free (no Circle headers, no <atomic>,
// no threads): synchronization is __atomic builtins only, so the same
// header compiles for the Pi kernel (ARM) and the host harnesses
// (x86) and is understood by ThreadSanitizer.
//
// Memory discipline (Nuked-mirror):
// - The producer writes an entry's payload, then RELEASE-stores the
//   in-index; the consumer ACQUIRE-loads the in-index before reading
//   payload. The consumer advances the out-index with a RELEASE
//   store after consuming; the producer ACQUIRE-loads the out-index
//   for its full check. Payload is never read before its release.
// - In/out indices live on separate 64-byte lines: the producer's
//   line (in-index) and the consumer's line (out-index) are never
//   shared, so no false sharing between the cores.
// - Full = (in+1)&mask == out, i.e. N-1 usable entries, mirroring
//   CRingBuffer's convention.
//
// Pending-scan safety (used by the H8-side SWP read hook): the scan
// reads only indices in [out_acquired, in_acquired). The producer
// cannot overwrite a scanned index without lapping the whole ring
// (N pushes), which requires consumer progress plus N pushes; the
// scan spans at most N iterations (~ns each) while pushes arrive at
// the firmware's W rate (~20k/s at E1M1 load) and drains cost a
// write_register each, so a lap during a scan is bounded away by
// orders of magnitude. The scan is therefore exact for the measured
// read pattern (cold, read-your-own-write).
//
// This header is also used directly by the host harnesses and unit
// tests, so the SPSC algorithm is tested in isolation, not only
// through the engine.

#ifndef _mu50spsc_h
#define _mu50spsc_h

#include <cstddef>
#include <cstdint>

namespace mu50spsc
{

// Relaxed spin hint: lets the other core make progress without a
// scheduler call. On the Pi this lowers to the ARM yield hint in M4;
// the host builds use the matching x86 pause.
inline void CpuRelax()
{
#if defined(__aarch64__)
	__asm__ volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
	__asm__ volatile("pause" ::: "memory");
#else
	// Unknown target: plain compiler barrier, still correct.
	__asm__ volatile("" ::: "memory");
#endif
}

template <class T, size_t N>
struct TSpscQueue
{
	static_assert((N & (N - 1)) == 0, "SPSC queue size must be a power of 2");
	static_assert(N >= 2, "SPSC queue needs at least 2 slots");

	TSpscQueue()
		: m_nIn(0),
		  m_nOut(0)
	{
	}

	// Producer only. False when full (N-1 usable entries).
	bool TryPush(const T& Item)
	{
		const size_t nIn  = m_nIn;
		const size_t nOut = __atomic_load_n(&m_nOut, __ATOMIC_ACQUIRE);
		if (((nIn + 1) & Mask) == nOut)
			return false;
		m_Data[nIn] = Item;
		__atomic_store_n(&m_nIn, (nIn + 1) & Mask, __ATOMIC_RELEASE);
		return true;
	}

	// Consumer only. False when empty.
	bool TryPop(T& OutItem)
	{
		const size_t nOut = m_nOut;
		const size_t nIn  = __atomic_load_n(&m_nIn, __ATOMIC_ACQUIRE);
		if (nOut == nIn)
			return false;
		OutItem = m_Data[nOut];
		__atomic_store_n(&m_nOut, (nOut + 1) & Mask, __ATOMIC_RELEASE);
		return true;
	}

	// Consumer only: copy the head entry without consuming it.
	// False when empty.
	bool TryPeek(T& OutItem)
	{
		const size_t nOut = m_nOut;
		const size_t nIn  = __atomic_load_n(&m_nIn, __ATOMIC_ACQUIRE);
		if (nOut == nIn)
			return false;
		OutItem = m_Data[nOut];
		return true;
	}

	// Consumer only: drop the head entry (used after a successful
	// TryPeek, and by the bounded drain).
	void Pop()
	{
		__atomic_store_n(&m_nOut, (m_nOut + 1) & Mask, __ATOMIC_RELEASE);
	}

	// Producer-side pending count (entries pushed but not drained).
	size_t PendingForProducer()
	{
		const size_t nIn  = m_nIn;
		const size_t nOut = __atomic_load_n(&m_nOut, __ATOMIC_ACQUIRE);
		return (nIn - nOut) & Mask;
	}

	// Consumer-side pending count.
	size_t PendingForConsumer()
	{
		const size_t nIn  = __atomic_load_n(&m_nIn, __ATOMIC_ACQUIRE);
		const size_t nOut = m_nOut;
		return (nIn - nOut) & Mask;
	}

	// Index snapshots for publish/drain bounds (block descriptors
	// carry the producer's in-index; the consumer drains up to it).
	size_t InIndex()
	{
		return __atomic_load_n(&m_nIn, __ATOMIC_ACQUIRE);
	}
	size_t OutIndex()
	{
		return __atomic_load_n(&m_nOut, __ATOMIC_ACQUIRE);
	}

	bool EmptyForConsumer()
	{
		return m_nOut == __atomic_load_n(&m_nIn, __ATOMIC_ACQUIRE);
	}

	// Consumer-side newest-wins scan (H8 SWP-read path): returns the
	// newest pending entry matching Pred, or false. The caller owns
	// the consumer side; the producer may push concurrently (see the
	// header note on scan safety).
	template <class Pred>
	bool FindNewest(Pred&& PredFn, T& OutItem)
	{
		const size_t nIn  = __atomic_load_n(&m_nIn, __ATOMIC_ACQUIRE);
		const size_t nOut = __atomic_load_n(&m_nOut, __ATOMIC_ACQUIRE);
		size_t nCount = (nIn - nOut) & Mask;
		size_t nIdx = (nIn + N - 1) & Mask;
		while (nCount-- > 0)
		{
			if (PredFn(m_Data[nIdx]))
			{
				OutItem = m_Data[nIdx];
				return true;
			}
			nIdx = (nIdx + N - 1) & Mask;
		}
		return false;
	}

	// Single-threaded reset (parked/quiescent only, never concurrent
	// with a producer or consumer).
	void Clear()
	{
		m_nIn  = 0;
		m_nOut = 0;
	}

	static constexpr size_t Mask = N - 1;

	T m_Data[N];
	// Separate cache lines: the producer writes m_nIn, the consumer
	// writes m_nOut; sharing a line would ping-pong every push/pop.
	alignas(64) size_t m_nIn;
	alignas(64) size_t m_nOut;
};

} // namespace mu50spsc

#endif
