#include <heaplayers.h>

#include <stdio.h>
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "stprintf.h"
#include "tprintf.h"
#include "common.hpp"

#include "mmaparray.hpp"
#include "sampleheap.hpp"

#include "repoman.hpp"
#include "reposource.hpp"

#include "fastmemcpy.hpp"



#if defined(__APPLE__)
#include "macinterpose.h"
#include "tprintf.h"
#endif

// We use prime numbers here (near 1MB, for example) to reduce the risk
// of stride behavior interfering with sampling.

const auto MallocSamplingRate = 1048549UL;
const auto MemcpySamplingRate = 2097131UL; // next prime after MallocSamplingRate * 2 + 1;
// TBD: use sampler logic (with random sampling) to obviate the need for primes
// already doing this for malloc-sampling.

#include "nextheap.hpp"

//class CustomHeapType : public NextHeap {
//class CustomHeapType : public HL::ThreadSpecificHeap<NextHeap> {
class CustomHeapType : public HL::ThreadSpecificHeap<SampleHeap<MallocSamplingRate, NextHeap>> {
public:
  void lock() {}
  void unlock() {}
};

//typedef NextHeap CustomHeapType;

class InitializeMe {
public:
  InitializeMe()
  {
#if 1
    // invoke backtrace so it resolves symbols now
#if 0 // defined(__linux__)
    volatile void * dl = dlopen("libgcc_s.so.1", RTLD_NOW | RTLD_GLOBAL);
#endif
    void * callstack[4];
    auto frames = backtrace(callstack, 4);
#endif
    //    isInitialized = true;
  }
};

static volatile InitializeMe initme;

static CustomHeapType thang;

#define getTheCustomHeap() thang

#if 0
CustomHeapType& getTheCustomHeap() {
  static CustomHeapType thang;
  return thang;
}
#endif


#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h> // for getpid()
#include <signal.h>

template <unsigned long MemcpySamplingRateBytes>
class MemcpySampler {
  enum { MemcpySignal = SIGPROF };
  static constexpr auto flags = O_WRONLY | O_CREAT | O_SYNC | O_APPEND; // O_TRUNC;
  static constexpr auto perms = S_IRUSR | S_IWUSR;
  static constexpr auto fname = "/tmp/scalene-memcpy-signalXXXXX";
public:
  MemcpySampler()
    : _interval (MemcpySamplingRateBytes),
      _memcpyOps (0),
      _memcpyTriggered (0)
  {
    signal(MemcpySignal, SIG_IGN);
    auto pid = getpid();
    int i;
    for (i = 0; i < local_strlen(fname); i++) {
      if (fname[i] == 'X') {
	break;
      }
      scalene_memcpy_signal_filename[i] = fname[i];
    }
    stprintf::stprintf((char *) &scalene_memcpy_signal_filename[i], "@", pid);
  }

  int local_strlen(const char * str) {
    int len = 0;
    while (*str != '\0') {
      len++;
      str++;
    }
    return len;
  }
  
  ~MemcpySampler() {
    unlink(scalene_memcpy_signal_filename);
  }

  ATTRIBUTE_ALWAYS_INLINE inline void * memcpy(void * dst, const void * src, size_t n) {
    auto result = local_memcpy(dst, src, n);
    incrementMemoryOps(n);
    return result; // always dst
  }

  ATTRIBUTE_ALWAYS_INLINE inline void * memmove(void * dst, const void * src, size_t n) {
    auto result = local_memmove(dst, src, n);
    incrementMemoryOps(n);
    return result; // always dst
  }

  ATTRIBUTE_ALWAYS_INLINE inline char * strcpy(char * dst, const char * src) {
    auto n = ::strlen(src);
    auto result = local_strcpy(dst, src);
    incrementMemoryOps(n);
    return result; // always dst
  }
  
private:

  //// local implementations of memcpy and friends.
  
  ATTRIBUTE_ALWAYS_INLINE inline void * local_memcpy(void * dst, const void * src, size_t n) {
#if defined(__APPLE__)
    return ::memcpy(dst, src, n);
#else
    return memcpy_fast(dst, src, n);
#endif
  }
  
  void * local_memmove(void * dst, const void * src, size_t n) {
#if defined(__APPLE__)
    return ::memmove(dst, src, n);
#else
    // TODO: optimize if these areas don't overlap.
    void * buf = malloc(n);
    local_memcpy(buf, src, n);
    local_memcpy(dst, buf, n);
    free(buf);
    return dst;
#endif
  }

  char * local_strcpy(char * dst, const char * src) {
    char * orig_dst = dst;
    while (*src != '\0') {
      *dst++ = *src++;
    }
    *dst = '\0';
    return orig_dst;
  }
  
  void incrementMemoryOps(int n) {
    _memcpyOps += n;
    if (unlikely(_memcpyOps >= _interval)) {
      writeCount();
      _memcpyTriggered++;
      _memcpyOps = 0;
      raise(MemcpySignal);
    }
  }
  
  unsigned long _memcpyOps;
  unsigned long long _memcpyTriggered;
  unsigned long _interval;
  char scalene_memcpy_signal_filename[255];

  void writeCount() {
    char buf[255];
    stprintf::stprintf(buf, "@,@\n", _memcpyTriggered, _memcpyOps);
    int fd = open(scalene_memcpy_signal_filename, flags, perms);
    write(fd, buf, strlen(buf));
    close(fd);
  }
};

auto& getSampler() {
  static MemcpySampler<MemcpySamplingRate> msamp;
  return msamp;
}

#if defined(__APPLE__)
#define LOCAL_PREFIX(x) xx##x
#else
#define LOCAL_PREFIX(x) x
#endif

extern "C" ATTRIBUTE_EXPORT void * LOCAL_PREFIX(memcpy)(void * dst, const void * src, size_t n) {
  // tprintf::tprintf("memcpy @ @ (@)\n", dst, src, n);
  auto result = getSampler().memcpy(dst, src, n);
  return result;
}

extern "C" ATTRIBUTE_EXPORT void * LOCAL_PREFIX(memmove)(void * dst, const void * src, size_t n) {
  auto result = getSampler().memmove(dst, src, n);
  return result;
}

extern "C" ATTRIBUTE_EXPORT char * LOCAL_PREFIX(strcpy)(char * dst, const char * src) {
  // tprintf::tprintf("strcpy @ @ (@)\n", dst, src);
  auto result = getSampler().strcpy(dst, src);
  return result;
}

extern "C" ATTRIBUTE_EXPORT void * xxmalloc(size_t sz) {
  void * ptr = nullptr;
  ptr = getTheCustomHeap().malloc(sz);
  return ptr;
}

extern "C" ATTRIBUTE_EXPORT void xxfree(void * ptr) {
  getTheCustomHeap().free(ptr);
}

extern "C" ATTRIBUTE_EXPORT void xxfree_sized(void * ptr, size_t sz) {
  // TODO FIXME maybe make a sized-free version?
  getTheCustomHeap().free(ptr);
}

extern "C" ATTRIBUTE_EXPORT size_t xxmalloc_usable_size(void * ptr) {
  return getTheCustomHeap().getSize(ptr); // TODO FIXME adjust for ptr offset?
}

extern "C" ATTRIBUTE_EXPORT void xxmalloc_lock() {
  getTheCustomHeap().lock();
}

extern "C" ATTRIBUTE_EXPORT void xxmalloc_unlock() {
  getTheCustomHeap().unlock();
}

#if defined(__APPLE__)
MAC_INTERPOSE(xxmemcpy, memcpy);
MAC_INTERPOSE(xxmemmove, memmove);
MAC_INTERPOSE(xxstrcpy, strcpy);
#endif
