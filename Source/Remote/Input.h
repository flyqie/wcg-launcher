#pragma once

#include "Rfb.h"

// A viewer's input, turned into input for the container: an X11 keysym per key and a
// button mask per pointer event, against Desktop's virtual keys and buttons. Also
// where the dock's hit test lives, since the dock is the client's own and a click on
// it must not reach the container.
//
// The On* pair are libvncserver's callbacks and only post; the Apply* pair are the
// frame loop's and are what touch the container. Handover holds the reason.
namespace Input
{
	void OnKeyboard(rfbBool bDown, rfbKeySym KeySym, rfbClientPtr cl);
	void OnPointer(int ButtonMask, int X, int Y, rfbClientPtr cl);

	// The frame loop's. Screen supplies the dock's width and height.
	void ApplyKey(unsigned long KeySym, bool bDown);
	void ApplyPointer(rfbScreenInfoPtr Screen, int ButtonMask, int X, int Y);

	// Recovers the shared injected state after queue overflow. The frame loop's.
	void ReleaseAll();

	// Where the viewer's pointer last was, in frame coordinates.
	void PointerPosition(int& X, int& Y);
}
