#pragma once

// Configuration that differs between deployments, all of it optional. Read once and
// kept for the life of the process, and nothing here fails: an unset or malformed value
// falls back to the default. See Docs/BUILD.md, "Configuration".

// Whether to serve the desktop over RFB at all, from LAUNCHER_VNC; off unless set.
//
// Off, the desktop and the program on it are still started, and the session still ends
// when that program does -- it just serves nothing, and captures nothing, since a frame
// nothing will be sent is the expensive half of the loop.
bool LauncherVnc();

// Port the RFB server listens on, from LAUNCHER_VNC_PORT.
int LauncherVncPort();

// Password a viewer has to give, from LAUNCHER_VNC_PW; empty when unset, which means no
// authentication at all.
//
// VNC derives its DES key from the first eight bytes, so a longer password is silently
// truncated by both ends.
const char* LauncherVncPassword();

// Requested display mode, from LAUNCHER_RESOLUTION as "WIDTHxHEIGHT". False when unset
// or malformed, leaving the container's default alone.
//
// The container has no display adapter, so win32k falls back to a stub driver that
// advertises a fixed set of 18 modes; anything else is rejected and logged. See
// Docs/IMAGE-ANALYSIS.md.
bool LauncherResolution(int& Width, int& Height);

// Whether the client draws its dock, from LAUNCHER_DOCK; off unless set. See Dock.h.
bool LauncherDock();

// A boolean environment variable: "1", "y" or "t" in either case is true, and anything
// else -- including unset -- is false.
bool LauncherFlag(const char* Name);
