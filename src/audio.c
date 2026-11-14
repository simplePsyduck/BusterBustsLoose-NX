#include <string.h>
#include <stdlib.h>
#include <SDL.h>
#include "audio.h"
#include "config.h"
#include "audioring.h"
#ifdef __SWITCH__
#include <switch.h>
#endif

static SDL_AudioDeviceID dev;
static AudioRing* ring;
static int16_t mixBuf[4096 * 2];
static float peakL, peakR;
static int targetQueued;     // frames of audio to keep queued
static int resumeFill;       // after a dry spell the device waits for this much
static double fillAvg;       // smoothed fill, for the rate control
static int callbackCount;

// the device asks for audio from its own thread: it takes what is queued, never waits for the game
static void SDLCALL audioCallback(void* user, Uint8* stream, int len) {
  (void)user;
#ifdef __SWITCH__
  static bool prioritised;
  if(!prioritised) {
    // this thread only copies a few KB: let it run ahead of the game's threads when it is due
    prioritised = true;
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);   // (refused: stays as it was)
  }
#endif
  ring_read(ring, (int16_t*)stream, len / 4, resumeFill);
}

bool audio_init(void) {
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = AUDIO_RATE;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = audioCallback;
  // keep ~4 frames of audio queued: enough to ride out hiccups, low latency
  targetQueued = (int)(AUDIO_RATE / SNES_FPS * 4);
  resumeFill = targetQueued / 2;
  ring = ring_new(16384);
  if(!ring) return false;
  dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if(!dev) { ring_free(ring); ring = NULL; return false; }
  fillAvg = targetQueued;
  SDL_PauseAudioDevice(dev, 0);
  return true;
}

void audio_shutdown(void) {
  if(dev) SDL_CloseAudioDevice(dev);
  dev = 0;
  ring_free(ring);
  ring = NULL;
}

int audio_queued(void) {
  return ring ? ring_fill(ring) : 0;
}

void audio_stats(int* fillFrames, int* minFill, int* maxFill, int* underruns, int* overruns) {
  AudioRingStats st = {0};
  if(ring) ring_stats(ring, &st, 1);
  *fillFrames = ring ? ring_fill(ring) : 0;
  *minFill = st.minFill;
  *maxFill = st.maxFill;
  *underruns = st.underruns;
  *overruns = st.overruns;
}

int audio_samplesForFrame(void) {
  double base = AUDIO_RATE / SNES_FPS;
  if(!dev) return (int)base;
  // nudge the rate by up to +-1% to keep the queue near its target. The fill is smoothed: the device
  // takes its audio in blocks, so the instantaneous value saws up and down by a block.
  static double frac = 0;
  fillAvg += (audio_queued() - fillAvg) * 0.1;
  double err = (targetQueued - fillAvg) / targetQueued;
  if(err > 1) err = 1;
  if(err < -1) err = -1;
  double want = base * (1.0 + 0.01 * err) + frac;
  int n = (int)want;
  frac = want - n;
  if(n > 4096) n = 4096;
  return n;
}

void audio_push(const int16_t* samples, int count) {
  if(!dev || count <= 0) return;
  if(count > 4096) count = 4096;
  int vol = cfg.volume * 256 / 100;
  int pl = 0, pr = 0;
  for(int i = 0; i < count; i++) {
    int l = samples[i * 2], r = samples[i * 2 + 1];
    if(cfg.monoAudio) l = r = (l + r) / 2;
    l = l * vol / 256;
    r = r * vol / 256;
    if(l > 32767) l = 32767; else if(l < -32768) l = -32768;
    if(r > 32767) r = 32767; else if(r < -32768) r = -32768;
    mixBuf[i * 2] = (int16_t)l;
    mixBuf[i * 2 + 1] = (int16_t)r;
    if(abs(l) > pl) pl = abs(l);
    if(abs(r) > pr) pr = abs(r);
  }
  peakL = pl / 32768.0f;
  peakR = pr / 32768.0f;
  ring_write(ring, mixBuf, count);   // (counted as an overrun if the ring is full)
}

void audio_pushSilence(int count) {
  if(!dev || count <= 0) return;
  if(count > 4096) count = 4096;
  if(audio_queued() > targetQueued) return;
  memset(mixBuf, 0, count * 4);
  peakL = peakR = 0;
  ring_write(ring, mixBuf, count);
}

void audio_clear(void) {
  if(ring) ring_flush(ring);
}

void audio_setPaused(bool paused) {
  if(dev) SDL_PauseAudioDevice(dev, paused ? 1 : 0);
}

void audio_levels(float* left, float* right) {
  *left = peakL;
  *right = peakR;
}
