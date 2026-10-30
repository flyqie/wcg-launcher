#pragma once

#include "Rfb.h"

namespace Input
{
	void OnKeyboard(rfbBool bDown, rfbKeySym KeySym, rfbClientPtr cl);
	void OnPointer(int ButtonMask, int X, int Y, rfbClientPtr cl);

	void ApplyKey(unsigned long KeySym, bool bDown);
	void ApplyPointer(rfbScreenInfoPtr Screen, int ButtonMask, int X, int Y);

	void ReleaseAll();

	void PointerPosition(int& X, int& Y);
}
