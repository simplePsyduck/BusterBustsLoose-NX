// Nintendo Switch (libnx / devkitA64) glue, see switch_platform.h
#ifdef __SWITCH__
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL.h>
#include "switch_platform.h"

void switch_init(void) {
#ifdef BBL_NXLINK
  // stdout/stderr go to `nxlink -s` on the PC (build with: make -f Makefile.switch NXLINK=1)
  socketInitializeDefault();
  nxlinkStdio();
#endif
}

void switch_shutdown(void) {
#ifdef BBL_NXLINK
  socketExit();
#endif
}

const char* switch_baseDir(int argc, char** argv) {
  static char dir[512];
  dir[0] = 0;
  if(argc > 0 && argv && argv[0] && argv[0][0]) {
    snprintf(dir, sizeof(dir), "%s", argv[0]);
    char* slash = strrchr(dir, '/');
    if(slash) slash[1] = 0; else dir[0] = 0;
  }
  // started loose from /switch (or with no usable path): keep our files in their own folder
  if(!dir[0] || !strcmp(dir, "sdmc:/switch/") || !strcmp(dir, "/switch/") || !strcmp(dir, "sdmc:/") || !strcmp(dir, "/")) {
    snprintf(dir, sizeof(dir), "%s", SWITCH_APP_DIR);
  }
  mkdir(dir, 0755);   // (already there in the normal case)
  return dir;
}

void switch_setupPads(void) {
  // Button order of SDL's Switch joystick driver: A B X Y, L3 R3, L R, ZL ZR,
  // + -, then D-pad left up right down; axes LX LY RX RY. The face buttons are
  // mapped by position (SDL "a" = the bottom button, which Nintendo labels B),
  // which is what config_defaultPad() assumes: SNES B <- bottom, SNES A <- right.
  // ZL/ZR are digital, so they become the trigger axes the port reads for fast forward.
  static const char tail[] =
    "a:b1,b:b0,x:b3,y:b2,"
    "back:b11,start:b10,"
    "leftshoulder:b6,rightshoulder:b7,lefttrigger:b8,righttrigger:b9,"
    "leftstick:b4,rightstick:b5,"
    "dpup:b13,dpdown:b15,dpleft:b12,dpright:b14,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,";
  int n = SDL_NumJoysticks();
  for(int i = 0; i < n; i++) {
    char guid[64], map[512];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
    snprintf(map, sizeof(map), "%s,Nintendo Switch,%s", guid, tail);
    SDL_GameControllerAddMapping(map);   // (replaces any built-in mapping for the same GUID)
  }
}

bool switch_confirmPressed(void) {
  static PadState pad;
  static bool ready;
  if(!ready) {
    padInitializeDefault(&pad);   // player 1 + handheld
    ready = true;
  }
  padUpdate(&pad);
  u64 down = padGetButtonsDown(&pad);
  return (down & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)) != 0;
}
#endif
