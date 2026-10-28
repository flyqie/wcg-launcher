#pragma once

// rfb.h before Windows.h, and the order matters: Windows.h pulls in winsock.h unless
// WIN32_LEAN_AND_MEAN is set, and winsock.h and winsock2.h cannot share a translation
// unit. rfb.h defines the guard that makes Windows.h skip its copy.
#include <rfb/rfb.h>

#include <Windows.h>
