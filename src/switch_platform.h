#pragma once
// Nintendo Switch (libnx / devkitA64) glue. Only built with __SWITCH__; the
// rest of the port stays plain SDL2 and goes through these few calls.
#ifdef __SWITCH__
#include <stdbool.h>

#define SWITCH_APP_DIR "sdmc:/switch/BusterBustsLoose/"

// libnx extras before SDL starts (nxlink stdout when built with NXLINK=1)
void switch_init(void);
void switch_shutdown(void);

// Folder for the ROM, bbl.ini and saves, with a trailing '/': the folder the
// .nro was started from (argv[0]), or SWITCH_APP_DIR when that is /switch itself
// or unknown. The folder is created if it is missing.
const char* switch_baseDir(int argc, char** argv);

// Registers the SDL game controller mapping for the console's pads (positional
// face buttons like the PC defaults: SDL A = bottom button). Safe to call again.
void switch_setupPads(void);

// True once A, B or + is pressed (read straight from libnx, so it works on the
// early error screen even if SDL's controller mapping is wrong)
bool switch_confirmPressed(void);
#endif
