#pragma once

#include "Rfb.h"

// The handover from libvncserver's threads to the frame loop.
//
// Nothing a callback does may touch the desktop. libvncserver delivers a viewer's input
// on threads of its own, and those are not on the desktop this program built:
// SetThreadDesktop is per-thread, and a thread created afterwards does not inherit its
// creator's. Measured -- a callback that injected directly gets ERROR_ACCESS_DENIED
// from SendInput, and SetCursorPos moves a cursor on a desktop nobody is looking at.
//
// So the callbacks post and the frame loop applies. What it carries belongs to Input and
// to Clipboard; it knows nothing about either beyond "apply what you were given".
namespace Handover
{
	// The wake the frame loop waits on, so input is applied on arrival rather than at the
	// next frame boundary. Null if it could not be made, in which case input is a frame
	// late. Handed out rather than waited on here because the loop waits on this and on
	// the frame timer at once.
	HANDLE Event();

	bool Open();
	void Close();

	// The callbacks' half, and the only thing here that runs on libvncserver's threads.
	void PostPointer(int ButtonMask, int X, int Y);
	void PostKey(unsigned long KeySym, bool bDown);
	void PostCutText(const char* Text, int Length);

	// Everything posted, applied in order. True if input was applied. The frame loop's.
	bool Apply(rfbScreenInfoPtr Screen);
}
