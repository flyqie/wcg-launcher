#pragma once

// Default RFB port, overridden at runtime by LAUNCHER_VNC_PORT. 5900 is VNC display :0,
// which is what a viewer assumes when it is given a bare address. See LauncherEnv.h.
#define LAUNCHER_VNC_PORT 5900
