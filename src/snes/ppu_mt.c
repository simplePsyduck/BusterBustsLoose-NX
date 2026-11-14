#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "snes.h"
#include "ppu.h"
#include "ppu_mt.h"

#define CHUNK_LINES 8
#define MAX_CHUNKS (PPU_BUF_H + 2)   // (never more than one chunk per line)
#define LOG_CAP 4096          // VRAM/CGRAM writes during one frame's picture (a resync happens when it fills up)

#define SNAP_OFF ((size_t)offsetof(Ppu, vramPointer))
#define SNAP_LEN ((size_t)offsetof(Ppu, ppuSnapLast) - SNAP_OFF)

typedef struct LineSnap {
  uint8_t obj[PPU_BUF_W];
  uint8_t objPrio[PPU_BUF_W];
  uint8_t* state;                  // SNAP_LEN bytes (allocated with the table)
} LineSnap;

typedef struct LogEntry {
  uint16_t stamp;                  // lines handed over before the write: it applies from line stamp + 1
  uint16_t kind, adr, val;
} LogEntry;

typedef struct Chunk { int first, last, logCount; } Chunk;

typedef struct Worker {
  PpuMt* mt;
  int id;
  void* thread;
  Ppu* rp;                         // this worker's PPU: private VRAM/CGRAM, registers copied per line
  int logPos;
  uint64_t busyNs;
} Worker;

struct PpuMt {
  Ppu* owner;
  int nw;
  Worker w[PPU_MT_MAX_WORKERS];
  Worker mainW;                    // the emulation thread draws too while it waits for the others
  void* mu;
  void* cvWork;                    // new chunk published / quit
  void* cvDone;                    // a chunk finished
  bool quit;
  uint32_t* out;                   // the finished picture, PPU_BUF_W x PPU_BUF_H
  LineSnap* slots;
  uint8_t* stateMem;
  Chunk chunks[MAX_CHUNKS];
  int chunksPublished, chunksClaimed, chunksDone;
  // emulation thread only
  bool inFrame;
  int last;                        // last line handed over in this frame
  int pendFirst;                   // first line of the chunk being filled (0 = none)
  int renderedLines;
  int tailBorderClear;
  bool tailPending;
  LogEntry* log;
  int logCount;                    // (also read by workers through the chunk records)
  // statistics / debugging
  bool verify, needCompare;
  uint64_t waitNs, helpNs;
  int frames;
  int verifyFrames, verifyBad, verifyFirstLine, verifyFirstX;
  int logOverflows;
};

static const PpuMtPlatform* pf;
void ppu_mtSetPlatform(const PpuMtPlatform* p) { pf = p; }

static uint64_t now(void) { return pf->nanos ? pf->nanos() : 0; }

// ------------------------------------------------------------------ worker side

static void applyLog(Worker* w, int line, int limit) {
  PpuMt* m = w->mt;
  Ppu* rp = w->rp;
  while(w->logPos < limit && m->log[w->logPos].stamp < line) {
    const LogEntry* e = &m->log[w->logPos++];
    if(e->kind == 0) rp->vram[e->adr & 0x7fff] = e->val;
    else rp->cgram[e->adr & 0xff] = e->val;
  }
}

static void drawChunk(Worker* w, const Chunk* ch) {
  PpuMt* m = w->mt;
  Ppu* rp = w->rp;
  for(int line = ch->first; line <= ch->last; line++) {
    applyLog(w, line, ch->logCount);
    const LineSnap* s = &m->slots[line - 1];
    memcpy((uint8_t*)rp + SNAP_OFF, s->state, SNAP_LEN);
    memcpy(rp->objPixelBuffer, s->obj, PPU_BUF_W);
    memcpy(rp->objPriorityBuffer, s->objPrio, PPU_BUF_W);
    ppu_renderLine(rp, line);
    uint32_t* dst = &m->out[(size_t)(line - 1) * PPU_BUF_W];
    const uint32_t* src = &rp->pixelBuffer[(size_t)(line - 1) * PPU_BUF_W];
    int x0 = PPU_MAX_EXTRA - rp->extraLeft, x1 = PPU_MAX_EXTRA + 256 + rp->extraRight;
    memcpy(dst + x0, src + x0, (size_t)(x1 - x0) * 4);
    int bc = rp->mtBorderClear;
    if(bc > 0) {
      memset(dst + PPU_MAX_EXTRA - bc, 0, (size_t)bc * 4);
      memset(dst + PPU_MAX_EXTRA + 256, 0, (size_t)bc * 4);
    }
  }
}

static int workerMain(void* arg) {
  Worker* w = arg;
  PpuMt* m = w->mt;
  pf->lock(m->mu);
  for(;;) {
    while(!m->quit && m->chunksClaimed >= m->chunksPublished) pf->wait(m->cvWork, m->mu);
    if(m->quit) break;
    Chunk ch = m->chunks[m->chunksClaimed++];
    pf->unlock(m->mu);
    uint64_t t0 = now();
    drawChunk(w, &ch);
    w->busyNs += now() - t0;
    pf->lock(m->mu);
    m->chunksDone++;
    pf->broadcast(m->cvDone);
  }
  pf->unlock(m->mu);
  return 0;
}

// ------------------------------------------------------------------ emulation side

// Waits until every published chunk is drawn. The time the emulation thread would sit idle is spent
// drawing chunks nobody has claimed yet (with its own private copy of the memory, like a worker).
static void waitIdle(PpuMt* m) {
  uint64_t t0 = now(), helped = 0;
  pf->lock(m->mu);
  while(m->chunksDone < m->chunksPublished) {
    if(m->chunksClaimed < m->chunksPublished) {
      Chunk ch = m->chunks[m->chunksClaimed++];
      pf->unlock(m->mu);
      uint64_t t1 = now();
      drawChunk(&m->mainW, &ch);
      helped += now() - t1;
      pf->lock(m->mu);
      m->chunksDone++;
    } else {
      pf->wait(m->cvDone, m->mu);
    }
  }
  pf->unlock(m->mu);
  m->helpNs += helped;
  m->waitNs += now() - t0 - helped;
}

static void publishPending(PpuMt* m) {
  if(m->pendFirst <= 0) return;
  pf->lock(m->mu);
  Chunk* c = &m->chunks[m->chunksPublished];
  c->first = m->pendFirst;
  c->last = m->last;
  c->logCount = m->logCount;
  m->chunksPublished++;
  pf->broadcast(m->cvWork);
  pf->unlock(m->mu);
  m->pendFirst = 0;
}

// workers are idle: give them the memory as it is now and start a fresh log
static void resync(PpuMt* m) {
  Ppu* p = m->owner;
  for(int i = 0; i <= m->nw; i++) {
    Worker* wk = i < m->nw ? &m->w[i] : &m->mainW;
    Ppu* rp = wk->rp;
    memcpy(rp->vram, p->vram, sizeof(p->vram));
    memcpy(rp->cgram, p->cgram, sizeof(p->cgram));
    memcpy(rp->bgRemapLineActive, p->bgRemapLineActive, sizeof(p->bgRemapLineActive));
    memcpy(rp->bgRemapLineContent, p->bgRemapLineContent, sizeof(p->bgRemapLineContent));
    memcpy(rp->bgRemapLineHud, p->bgRemapLineHud, sizeof(p->bgRemapLineHud));
    for(int y = 0; y < PPU_BUF_H; y++)
      if(p->bgRemapLineActive[y]) memcpy(rp->bgRemapSrc[y], p->bgRemapSrc[y], sizeof(p->bgRemapSrc[y]));
    wk->logPos = 0;
  }
  m->logCount = 0;
  pf->lock(m->mu);                 // (the workers read these under the lock)
  m->chunksPublished = m->chunksClaimed = m->chunksDone = 0;
  pf->unlock(m->mu);
}

static void compareFrame(PpuMt* m) {
  m->needCompare = false;
  Ppu* p = m->owner;
  m->verifyFrames++;
  if(memcmp(m->out, p->pixelBuffer, sizeof(p->pixelBuffer)) == 0) return;
  m->verifyBad++;
  if(m->verifyBad == 1) {
    for(int i = 0; i < PPU_BUF_W * PPU_BUF_H; i++) {
      if(m->out[i] != p->pixelBuffer[i]) { m->verifyFirstLine = i / PPU_BUF_W + 1; m->verifyFirstX = i % PPU_BUF_W; break; }
    }
  }
}

void ppu_mtSync(Ppu* p) {
  PpuMt* m = p->mt;
  if(!m) return;
  waitIdle(m);
  if(m->tailPending) {
    m->tailPending = false;
    int bc = m->tailBorderClear;
    if(bc > 0) {
      for(int y = m->renderedLines; y < PPU_BUF_H; y++) {
        uint32_t* d = &m->out[(size_t)y * PPU_BUF_W];
        memset(d + PPU_MAX_EXTRA - bc, 0, (size_t)bc * 4);
        memset(d + PPU_MAX_EXTRA + 256, 0, (size_t)bc * 4);
      }
    }
  }
  if(m->needCompare) compareFrame(m);
}

const uint32_t* ppu_mtPixels(Ppu* p) {
  ppu_mtSync(p);
  return p->mt->out;
}

static void beginFrame(PpuMt* m, int firstLine) {
  publishPending(m);
  ppu_mtSync(m->owner);            // (also finishes the previous frame's bookkeeping)
  resync(m);
  m->inFrame = true;
  m->last = firstLine - 1;
  m->pendFirst = 0;
}

void ppu_mtCaptureLine(Ppu* p, int line) {
  PpuMt* m = p->mt;
  if(line < 1 || line > PPU_BUF_H) return;
  if(!m->inFrame || line != m->last + 1) beginFrame(m, line);
  LineSnap* s = &m->slots[line - 1];
  memcpy(s->state, (const uint8_t*)p + SNAP_OFF, SNAP_LEN);
  memcpy(s->obj, p->objPixelBuffer, PPU_BUF_W);
  memcpy(s->objPrio, p->objPriorityBuffer, PPU_BUF_W);
  if(m->pendFirst <= 0) m->pendFirst = line;
  m->last = line;
  if(m->verify) ppu_renderLine(p, line);
  if(line - m->pendFirst + 1 >= CHUNK_LINES) publishPending(m);
}

void ppu_mtEndFrame(Ppu* p) {
  PpuMt* m = p->mt;
  if(!m || !m->inFrame) return;
  publishPending(m);
  m->inFrame = false;
  m->renderedLines = m->last;
  m->tailBorderClear = p->mtBorderClear;
  m->tailPending = true;
  m->needCompare = m->verify;
  m->frames++;
}

void ppu_mtLog(Ppu* p, int kind, uint16_t adr, uint16_t val) {
  PpuMt* m = p->mt;
  if(!m->inFrame) return;
  if(m->logCount >= LOG_CAP) {
    // too many writes for the log (a game streaming data in with the screen on): let the workers
    // finish what they were given, then start again from the memory as it is now (this write is in it)
    m->logOverflows++;
    publishPending(m);
    waitIdle(m);
    resync(m);
    return;
  }
  LogEntry* e = &m->log[m->logCount++];
  e->stamp = (uint16_t)m->last;
  e->kind = (uint16_t)kind;
  e->adr = adr;
  e->val = val;
}

bool ppu_mtStart(Ppu* p, int workers) {
  if(p->mt) ppu_mtStop(p);
  if(workers <= 0) return true;
  if(!pf || !pf->threadStart || !pf->mutexNew || !pf->condNew) return false;
  if(workers > PPU_MT_MAX_WORKERS) workers = PPU_MT_MAX_WORKERS;
  PpuMt* m = calloc(1, sizeof(PpuMt));
  if(!m) return false;
  m->owner = p;
  m->nw = workers;
  m->out = malloc(sizeof(p->pixelBuffer));
  m->slots = calloc(PPU_BUF_H, sizeof(LineSnap));
  m->stateMem = malloc(PPU_BUF_H * SNAP_LEN);
  m->log = malloc(LOG_CAP * sizeof(LogEntry));
  m->mu = pf->mutexNew();
  m->cvWork = pf->condNew();
  m->cvDone = pf->condNew();
  bool ok = m->out && m->slots && m->stateMem && m->log && m->mu && m->cvWork && m->cvDone;
  if(ok) {
    memcpy(m->out, p->pixelBuffer, sizeof(p->pixelBuffer));
    for(int i = 0; i < PPU_BUF_H; i++) m->slots[i].state = m->stateMem + (size_t)i * SNAP_LEN;
    for(int i = 0; i < workers && ok; i++) {
      Worker* w = &m->w[i];
      w->mt = m;
      w->id = i;
      w->rp = calloc(1, sizeof(Ppu));
      if(!w->rp) { ok = false; break; }
      memcpy(w->rp, p, sizeof(Ppu));      // (colour profile, widescreen settings, ...; the snapshot refreshes them)
      w->rp->mt = NULL;
      w->rp->vramWriteHook = NULL;
      w->rp->spriteXHook = NULL;
    }
    m->mainW.mt = m;
    m->mainW.id = workers;
    m->mainW.rp = ok ? calloc(1, sizeof(Ppu)) : NULL;
    if(m->mainW.rp) {
      memcpy(m->mainW.rp, p, sizeof(Ppu));
      m->mainW.rp->mt = NULL;
      m->mainW.rp->vramWriteHook = NULL;
      m->mainW.rp->spriteXHook = NULL;
    } else ok = false;
  }
  int started = 0;
  if(ok) {
    p->mt = m;
    for(int i = 0; i < workers; i++) {
      m->w[i].thread = pf->threadStart(workerMain, &m->w[i], "bbl-ppu", i);
      if(!m->w[i].thread) break;
      started++;
    }
    if(started < workers) ok = false;
  }
  if(!ok) {
    // stop whatever did start
    pf->lock(m->mu);
    m->quit = true;
    pf->broadcast(m->cvWork);
    pf->unlock(m->mu);
    for(int i = 0; i < started; i++) pf->threadJoin(m->w[i].thread);
    for(int i = 0; i < workers; i++) free(m->w[i].rp);
    free(m->mainW.rp);
    if(m->mu) pf->mutexFree(m->mu);
    if(m->cvWork) pf->condFree(m->cvWork);
    if(m->cvDone) pf->condFree(m->cvDone);
    free(m->out); free(m->slots); free(m->stateMem); free(m->log); free(m);
    p->mt = NULL;
    return false;
  }
  return true;
}

void ppu_mtStop(Ppu* p) {
  PpuMt* m = p->mt;
  if(!m) return;
  publishPending(m);
  ppu_mtSync(p);
  memcpy(p->pixelBuffer, m->out, sizeof(p->pixelBuffer));   // keep showing the last picture
  pf->lock(m->mu);
  m->quit = true;
  pf->broadcast(m->cvWork);
  pf->unlock(m->mu);
  for(int i = 0; i < m->nw; i++) {
    pf->threadJoin(m->w[i].thread);
    free(m->w[i].rp);
  }
  free(m->mainW.rp);
  pf->mutexFree(m->mu);
  pf->condFree(m->cvWork);
  pf->condFree(m->cvDone);
  free(m->out); free(m->slots); free(m->stateMem); free(m->log);
  free(m);
  p->mt = NULL;
}

int ppu_mtWorkers(Ppu* p) { return p->mt ? p->mt->nw : 0; }

void ppu_mtSetVerify(Ppu* p, bool on) {
  if(p->mt) { ppu_mtSync(p); p->mt->verify = on; p->mt->needCompare = false; }
}

bool ppu_mtVerifying(Ppu* p) { return p->mt && p->mt->verify; }

void ppu_mtGetStats(Ppu* p, PpuMtStats* o, bool reset) {
  memset(o, 0, sizeof(*o));
  PpuMt* m = p->mt;
  if(!m) return;
  o->workers = m->nw;
  o->frames = m->frames;
  double perFrame = m->frames > 0 ? 1.0 / m->frames : 0;
  for(int i = 0; i < m->nw; i++) o->workerBusyMs[i] = m->w[i].busyNs * 1e-6 * perFrame;
  o->mainWaitMs = m->waitNs * 1e-6 * perFrame;
  o->mainHelpMs = m->helpNs * 1e-6 * perFrame;
  o->verifyFrames = m->verifyFrames;
  o->verifyBadFrames = m->verifyBad;
  o->verifyFirstLine = m->verifyFirstLine;
  o->verifyFirstX = m->verifyFirstX;
  o->logOverflows = m->logOverflows;
  if(reset) {
    for(int i = 0; i < m->nw; i++) m->w[i].busyNs = 0;
    m->waitNs = 0;
    m->helpNs = 0;
    m->frames = 0;
  }
}
