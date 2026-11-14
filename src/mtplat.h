#pragma once
// Thread primitives for the threaded PPU renderer (snes/ppu_mt.c): SDL threads, and on the Switch the
// workers are pinned to the cores the emulation thread is not on.
// Call once, from the emulation (main) thread, before ppu_mtStart().
void mtplat_init(void);
// the most render threads worth starting on this machine (Switch: 2, the other two of the three
// cores an application may use; elsewhere: cores - 1, at most 3)
int mtplat_maxWorkers(void);
