#ifndef PPU_MT_H
#define PPU_MT_H
// Threaded scanline rendering for the PPU.
//
// Emulation stays on the calling thread: at the point where the original code drew a scanline,
// the register state of that line (plus its sprite line buffer) is copied into a slot and handed to
// worker threads, which draw it from a private copy of VRAM/CGRAM while the emulation goes on with
// the next line. VRAM/CGRAM writes during the picture are logged with the line they happened on,
// and the workers replay them in order, so every line sees exactly the memory the synchronous
// renderer would have seen. The picture is bit-identical to the synchronous renderer
// (tests/perf/mt_fuzz.c checks that, and "MT verify" in the Video menu checks it on the real game).
//
// This file has no OS dependencies: the frontend supplies the thread primitives.
#include <stdint.h>
#include <stdbool.h>

typedef struct Ppu Ppu;

typedef struct PpuMtPlatform {
  void* (*mutexNew)(void);
  void (*mutexFree)(void* m);
  void (*lock)(void* m);
  void (*unlock)(void* m);
  void* (*condNew)(void);
  void (*condFree)(void* c);
  void (*wait)(void* c, void* m);          // m is locked by the caller; returns with it locked
  void (*broadcast)(void* c);
  void* (*threadStart)(int (*fn)(void*), void* arg, const char* name, int workerIndex);
  void (*threadJoin)(void* t);
  uint64_t (*nanos)(void);                 // monotonic clock (only used for the statistics)
} PpuMtPlatform;

#define PPU_MT_MAX_WORKERS 3

typedef struct PpuMtStats {
  int workers;
  double workerBusyMs[PPU_MT_MAX_WORKERS];   // per frame, averaged since the last reset
  double mainWaitMs;                         // per frame: time the emulation thread sat idle waiting for workers
  double mainHelpMs;                         // per frame: time it spent drawing lines itself while it would have waited
  int frames;
  int verifyFrames;                          // frames compared against the synchronous renderer
  int verifyBadFrames;                       // ...of which differed
  int verifyFirstLine, verifyFirstX;         // first differing pixel of the first bad frame (line 1-based, x in buffer columns)
  int logOverflows;
} PpuMtStats;

void ppu_mtSetPlatform(const PpuMtPlatform* p);
// Starts `workers` (1..PPU_MT_MAX_WORKERS) render threads for this PPU; false (and stays synchronous) on failure.
// Call between frames. Calling it again changes the number of workers (0 = back to synchronous).
bool ppu_mtStart(Ppu* ppu, int workers);
void ppu_mtStop(Ppu* ppu);
int ppu_mtWorkers(Ppu* ppu);
// waits until every handed-over line is drawn (and, with verify, compares the picture)
void ppu_mtSync(Ppu* ppu);
// debug: also draw every line synchronously and compare the whole picture after each frame
void ppu_mtSetVerify(Ppu* ppu, bool on);
bool ppu_mtVerifying(Ppu* ppu);          // (then the synchronous picture in ppu->pixelBuffer has to be finished like before)
void ppu_mtGetStats(Ppu* ppu, PpuMtStats* out, bool reset);

// used by ppu.c
void ppu_mtCaptureLine(Ppu* ppu, int line);
void ppu_mtEndFrame(Ppu* ppu);
void ppu_mtLog(Ppu* ppu, int kind, uint16_t adr, uint16_t val);   // kind 0 = VRAM word, 1 = CGRAM word
const uint32_t* ppu_mtPixels(Ppu* ppu);
#endif
