#pragma once

#include "Rfb.h"

namespace Handover
{
	HANDLE Event();

	bool Open();
	void Close();

	void PostPointer(int ButtonMask, int X, int Y);
	void PostKey(unsigned long KeySym, bool bDown);
	void PostCutText(const char* Text, int Length);

	bool Apply(rfbScreenInfoPtr Screen);
}
