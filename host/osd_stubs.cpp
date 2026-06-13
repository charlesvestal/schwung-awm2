// osd_stubs.cpp — definitions for OSD-provided free functions that the MAME
// core/frontend reference but that live in the SDL OSD objects we deliberately
// exclude from the headless link. Providing strong definitions here keeps the
// SDL window/osd objects (and thus libSDL3) out of the binary.

#include <cstdlib>
#include "modules/osdwindow.h"

// MAME's OSD-core clipboard helper (osdlib_unix) calls into SDL on Linux. We
// don't link SDL, and the module never uses the clipboard, so satisfy these few
// SDL symbols with no-op stubs (C ABI). Without them, dlopen() of dsp.so fails
// with "undefined symbol: SDL_GetClipboardText".
extern "C" {
char *SDL_GetClipboardText(void)        { char *p = (char *)std::malloc(1); if (p) p[0] = 0; return p; }
int   SDL_SetClipboardText(const char *) { return 0; }
int   SDL_HasClipboardText(void)        { return 0; }
void  SDL_free(void *p)                 { std::free(p); }
}

// The base osd_window code references the global `video_config`, normally defined
// in the SDL osd's video.o. Defining it here keeps the SDL video/window/osd
// objects out of the link. Zero-initialised; unused in the windowless run.
osd_video_config video_config;

// Referenced by the Lua UI binding (luaengine.cpp). No-op headless.
void osd_set_aggressive_input_focus(bool /*aggressive_focus*/) {}

// Referenced by the frontend UI (ui.cpp, miscmenu.cpp). The real version (in the
// SDL osd's sdlopts.o) adds SDL/window-specific options and would drag in the
// SDL video/window/osd objects; we add no extra options in the headless build.
class emu_options;
void osd_setup_osd_specific_emu_options(emu_options &) {}
