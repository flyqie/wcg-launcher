#pragma once

#include "Rfb.h"

#include "../Desktop.h"

namespace Frame
{
	void ApplyServerFormat(rfbScreenInfoPtr Screen);

	void Attach(rfbScreenInfoPtr Screen, int Width, int Height);

	int Update(rfbScreenInfoPtr Screen, const Desktop::Frame& Captured,
		unsigned long long& ChangedArea);
}
