#pragma once

#include "Rfb.h"

#include "../Desktop.h"

// The picture libvncserver serves, and the diff that decides what of it is new.
//
// The buffer is owned here rather than pointed at Desktop's DIB: that DIB is reused and
// reallocated when the display mode changes, and libvncserver reads the framebuffer
// from its own threads, so the pointer has to stay valid and stay the same size for as
// long as the screen does. It is also the reference the next capture is diffed against,
// which is what makes one buffer do the work of two.
namespace Frame
{
	// States the pixel format of a screen whose framebuffer is 32bpp BGRA. libvncserver
	// infers RGBA from the byte count alone, which would swap red and blue, and a GDI
	// DIB section is BGRA. Needed again after rfbNewFramebuffer, which re-infers.
	void ApplyServerFormat(rfbScreenInfoPtr Screen);

	// Gives a new screen a framebuffer of its own, at this size, cleared to black.
	void Attach(rfbScreenInfoPtr Screen, int Width, int Height);

	// Diffs a captured frame against what was last served, copies what changed into the
	// framebuffer and marks the rectangles covering it. Returns the number of rectangles
	// marked -- zero when nothing changed -- and reports the area they cover, which is
	// what the encoder and the link pay for.
	int Update(rfbScreenInfoPtr Screen, const Desktop::Frame& Captured,
		unsigned long long& ChangedArea);
}
