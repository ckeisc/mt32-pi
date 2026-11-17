// Host stub for <circle/memory.h>.
#ifndef ENGSTUB_CIRCLE_MEMORY_H
#define ENGSTUB_CIRCLE_MEMORY_H
#define HEAP_LOW 0
class CMemorySystem {
public:
  static CMemorySystem *Get() { static CMemorySystem s; return &s; }
  unsigned GetHeapFreeSpace(int) const { return 128u * 1024u * 1024u; }
};
#endif
