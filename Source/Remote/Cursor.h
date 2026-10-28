#pragma once

#include "Rfb.h"

#include "../Desktop.h"

// The container's cursor, published as an RFB cursor rather than drawn into the
// framebuffer: RFB carries it separately and the viewer composites it, so moving the
// mouse does not dirty the pixels underneath.
namespace Cursor
{
	void Publish(rfbScreenInfoPtr Screen, const Desktop::Cursor& Cursor);
}
