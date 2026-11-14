#include <stdlib.h>
#include <string.h>
#include <SDL_atomic.h>
#include "audioring.h"

#define FADE 96

struct AudioRing {
  int16_t* buf;
  int cap;                       // frames; power of two
  SDL_atomic_t w, r;             // total frames written / read (wrapping counters)
  SDL_atomic_t flush;
  SDL_atomic_t underruns, overruns, callbacks, minFill, maxFill;
  // consumer only
  bool rebuffering;
  int16_t lastL, lastR;
};

AudioRing* ring_new(int capFrames) {
  int cap = 1;
  while(cap < capFrames) cap <<= 1;
  AudioRing* q = calloc(1, sizeof(AudioRing));
  if(!q) return NULL;
  q->buf = calloc((size_t)cap * 2, sizeof(int16_t));
  if(!q->buf) { free(q); return NULL; }
  q->cap = cap;
  SDL_AtomicSet(&q->minFill, 1 << 30);
  q->rebuffering = true;   // (starts by filling up)
  return q;
}

void ring_free(AudioRing* q) {
  if(!q) return;
  free(q->buf);
  free(q);
}

int ring_fill(AudioRing* q) {
  int w = SDL_AtomicGet(&q->w), r = SDL_AtomicGet(&q->r);
  int f = (int)((unsigned)(w - r));
  return f < 0 ? 0 : (f > q->cap ? q->cap : f);
}

int ring_write(AudioRing* q, const int16_t* s, int frames) {
  int w = SDL_AtomicGet(&q->w);
  int free_ = q->cap - ring_fill(q);
  if(frames > free_) {
    SDL_AtomicAdd(&q->overruns, frames - free_);
    frames = free_;
  }
  int mask = q->cap - 1;
  for(int i = 0; i < frames; i++) {
    int p = (w + i) & mask;
    q->buf[p * 2] = s[i * 2];
    q->buf[p * 2 + 1] = s[i * 2 + 1];
  }
  SDL_MemoryBarrierRelease();
  SDL_AtomicSet(&q->w, w + frames);
  return frames;
}

void ring_flush(AudioRing* q) { SDL_AtomicSet(&q->flush, 1); }

static void fadeTail(AudioRing* q, int16_t* out, int frames) {
  // from the last sample played down to silence, then silence
  for(int i = 0; i < frames; i++) {
    int k = i < FADE ? FADE - 1 - i : 0;   // (the first one is already below the last played sample)
    out[i * 2] = (int16_t)(q->lastL * k / FADE);
    out[i * 2 + 1] = (int16_t)(q->lastR * k / FADE);
  }
}

void ring_read(AudioRing* q, int16_t* out, int frames, int resumeFill) {
  SDL_AtomicAdd(&q->callbacks, 1);
  if(SDL_AtomicGet(&q->flush)) {
    SDL_AtomicSet(&q->flush, 0);
    SDL_AtomicSet(&q->r, SDL_AtomicGet(&q->w));
    q->rebuffering = true;
  }
  int fill = ring_fill(q);
  if(fill < SDL_AtomicGet(&q->minFill)) SDL_AtomicSet(&q->minFill, fill);
  if(fill > SDL_AtomicGet(&q->maxFill)) SDL_AtomicSet(&q->maxFill, fill);
  SDL_MemoryBarrierAcquire();
  int mask = q->cap - 1;
  int r = SDL_AtomicGet(&q->r);
  int done = 0;
  if(q->rebuffering) {
    if(fill >= resumeFill && fill >= frames) q->rebuffering = false;
    else { fadeTail(q, out, frames); q->lastL = q->lastR = 0; return; }
  }
  int n = fill < frames ? fill : frames;
  for(int i = 0; i < n; i++) {
    int p = (r + i) & mask;
    out[i * 2] = q->buf[p * 2];
    out[i * 2 + 1] = q->buf[p * 2 + 1];
  }
  done = n;
  if(n > 0) { q->lastL = out[(n - 1) * 2]; q->lastR = out[(n - 1) * 2 + 1]; }
  SDL_MemoryBarrierRelease();
  SDL_AtomicSet(&q->r, r + n);
  if(done < frames) {
    // ran dry: finish the buffer with a fade-out and wait until there is enough again
    SDL_AtomicAdd(&q->underruns, 1);
    fadeTail(q, out + done * 2, frames - done);
    q->lastL = q->lastR = 0;
    q->rebuffering = true;
  }
}

void ring_stats(AudioRing* q, AudioRingStats* o, int reset) {
  o->underruns = SDL_AtomicGet(&q->underruns);
  o->overruns = SDL_AtomicGet(&q->overruns);
  o->callbacks = SDL_AtomicGet(&q->callbacks);
  int mn = SDL_AtomicGet(&q->minFill);
  o->minFill = mn == (1 << 30) ? 0 : mn;
  o->maxFill = SDL_AtomicGet(&q->maxFill);
  if(reset) {
    SDL_AtomicSet(&q->underruns, 0);
    SDL_AtomicSet(&q->overruns, 0);
    SDL_AtomicSet(&q->callbacks, 0);
    SDL_AtomicSet(&q->minFill, 1 << 30);
    SDL_AtomicSet(&q->maxFill, 0);
  }
}
