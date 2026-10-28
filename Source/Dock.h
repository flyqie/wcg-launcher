#pragma once

#include <Windows.h>

// The dock: the strip of window buttons along the bottom of the desktop.
//
// A container has no shell -- no taskbar, no Alt+Tab -- and the capture skips minimized
// windows because they have no pixels to paint, so without this a minimized window is
// gone for good. It behaves the way a taskbar does in both directions: drawn over the
// bottom of the desktop, so the frame is exactly the screen and the viewer's window is
// the same size either way; and reserving the same rows out of the work area, so
// maximized windows stop above it and the container sees a smaller usable area rather
// than a smaller screen.
//
// With LAUNCHER_DOCK unset every function here does nothing and Contains is always
// false, so no caller needs a branch of its own.
#define LAUNCHER_DOCK_HEIGHT 26

namespace Dock
{
	// Reserves the bottom of the work area, if LAUNCHER_DOCK asked for it. Call it after
	// the display mode is settled and before the program starts, so the program is born
	// knowing how much room it has.
	void Open();

	void Draw(HDC Dc, int Width, int Height);

	// True when Y, in frame coordinates, is over the dock.
	bool Contains(int Y, int FrameHeight);

	void Press(int X, int Y, int Width, int FrameHeight);
}
