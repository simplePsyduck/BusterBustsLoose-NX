#include <stdlib.h>
#include <SDL.h>
#include "mtplat.h"
#include "ppu_mt.h"
#ifdef __SWITCH__
#include <switch.h>
#endif

typedef struct Thr {
  SDL_Thread* t;
  int (*fn)(void*);
  void* arg;
  int index;
} Thr;

#ifdef __SWITCH__
static int mainCore = 0;

// an application may use cores 0-2 (core 3 belongs to the system). The workers take the two the
// emulation thread is not on, so nothing shares a core with it.
static void pinToCore(int workerIndex) {
  int core = -1, n = 0;
  for(int c = 0; c < 3; c++) {
    if(c == mainCore) continue;
    if(n++ == workerIndex) { core = c; break; }
  }
  if(core < 0) return;
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);   // (failure: the thread just floats)
}
#endif

static int threadEntry(void* a) {
  Thr* t = a;
#ifdef __SWITCH__
  pinToCore(t->index);
#endif
  return t->fn(t->arg);
}

static void* pMutexNew(void) { return SDL_CreateMutex(); }
static void pMutexFree(void* m) { SDL_DestroyMutex(m); }
static void pLock(void* m) { SDL_LockMutex(m); }
static void pUnlock(void* m) { SDL_UnlockMutex(m); }
static void* pCondNew(void) { return SDL_CreateCond(); }
static void pCondFree(void* c) { SDL_DestroyCond(c); }
static void pWait(void* c, void* m) { SDL_CondWait(c, m); }
static void pBroadcast(void* c) { SDL_CondBroadcast(c); }

static void* pThreadStart(int (*fn)(void*), void* arg, const char* name, int index) {
  Thr* t = calloc(1, sizeof(Thr));
  if(!t) return NULL;
  t->fn = fn;
  t->arg = arg;
  t->index = index;
  // (an explicit stack: the console's default for SDL threads is small, and a line's drawing uses a good few KB)
  t->t = SDL_CreateThreadWithStackSize(threadEntry, name, 512 * 1024, t);
  if(!t->t) { free(t); return NULL; }
  return t;
}

static void pThreadJoin(void* h) {
  Thr* t = h;
  SDL_WaitThread(t->t, NULL);
  free(t);
}

static uint64_t pNanos(void) {
  return (uint64_t)((double)SDL_GetPerformanceCounter() * 1e9 / (double)SDL_GetPerformanceFrequency());
}

void mtplat_init(void) {
  static const PpuMtPlatform plat = {
    pMutexNew, pMutexFree, pLock, pUnlock, pCondNew, pCondFree, pWait, pBroadcast, pThreadStart, pThreadJoin, pNanos
  };
#ifdef __SWITCH__
  mainCore = (int)svcGetCurrentProcessorNumber();
  if(mainCore < 0 || mainCore > 2) mainCore = 0;
#endif
  ppu_mtSetPlatform(&plat);
}

int mtplat_maxWorkers(void) {
#ifdef __SWITCH__
  return 2;
#else
  int n = SDL_GetCPUCount() - 1;
  if(n < 0) n = 0;
  if(n > PPU_MT_MAX_WORKERS) n = PPU_MT_MAX_WORKERS;
  return n;
#endif
}
