#pragma once

#include "Rfb.h"

#include "../Desktop.h"

namespace Cursor
{
	void Publish(rfbScreenInfoPtr Screen, const Desktop::Cursor& Cursor);
}
