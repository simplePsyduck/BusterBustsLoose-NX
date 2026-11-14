#pragma once
// Single-producer / single-consumer ring of stereo int16 frames between the emulation thread
// (writes a frame's audio) and the audio device's callback thread (reads what the device needs).
// No locks: the callback never waits for the emulation thread.
#include <stdint.h>
#include <stdbool.h>

typedef struct AudioRing AudioRing;

typedef struct AudioRingStats {
  int underruns;      // times the device wanted more than there was
  int overruns;       // frames of audio that did not fit and were dropped
  int minFill, maxFill;   // lowest / highest fill seen by the callback (frames)
  int callbacks;
} AudioRingStats;

AudioRing* ring_new(int capFrames);
void ring_free(AudioRing* r);
int ring_fill(AudioRing* r);                                    // frames waiting (either thread)
// producer: returns the number of frames stored (fewer when full)
int ring_write(AudioRing* r, const int16_t* stereo, int frames);
// producer: ask the consumer to throw away what is queued (done by the callback, at its next call)
void ring_flush(AudioRing* r);
// consumer: after an underrun it outputs silence until `resumeFill` frames are queued again
// (a stream that keeps running dry would otherwise click on every callback), with short fades
void ring_read(AudioRing* r, int16_t* stereo, int frames, int resumeFill);
// statistics since the last call with reset; either thread
void ring_stats(AudioRing* r, AudioRingStats* out, int reset);
