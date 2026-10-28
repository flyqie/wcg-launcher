#include "Cursor.h"

#include <iostream>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "../Common/LauncherLog.h"
#include "Input.h"

using namespace std;

namespace
{
	// The last bitmap read out of the container, and its metrics. Kept because ReadCursor
	// reads the bitmap only when the handle changes, so a frame in which the cursor merely
	// became visible again has no shape of its own and has to reuse this.
	vector<unsigned char> gShape;
	unsigned gWidth = 0;
	unsigned gHeight = 0;
	unsigned gHotX = 0;
	unsigned gHotY = 0;

	// Whether a cursor has been sent, and whether it was a visible one: a new bitmap and a
	// cursor that appeared or disappeared are different messages.
	bool gPublished = false;
	bool gVisible = false;
}

void Cursor::Publish(rfbScreenInfoPtr Screen, const Desktop::Cursor& Cursor)
{
	if (Cursor.ShapeChanged && !Cursor.Shape.empty())
	{
		gShape = Cursor.Shape;
		gWidth = Cursor.Width;
		gHeight = Cursor.Height;
		gHotX = Cursor.HotX;
		gHotY = Cursor.HotY;
	}

	// Until the container has shown a cursor once there is nothing to publish, and
	// publishing "draw nothing" is not the same as publishing nothing at all: the first
	// tells the viewer to stop drawing its own, and then there is neither. A fresh session
	// reads no cursor for its first frame or two, the desktop having only just been built.
	if (gShape.empty())
	{
		return;
	}

	// A cursor the container has stopped showing goes out as a one-pixel cursor with an
	// empty mask, which is how libvncserver spells "draw nothing" -- and once a shape has
	// been seen, the viewer has been told to hide its own and something must replace it.
	const bool bHaveShape = Cursor.Visible;
	const bool bNewShape = Cursor.ShapeChanged && !Cursor.Shape.empty();

	if (gPublished && !bNewShape && bHaveShape == gVisible)
	{
		return;
	}

	const unsigned Width = bHaveShape ? gWidth : 1;
	const unsigned Height = bHaveShape ? gHeight : 1;

	// How many pixels the mask ends up drawing, for the report below.
	unsigned Opaque = 0;

	const int MaskStride = ((int)Width + 7) / 8;

	rfbCursorPtr Out = (rfbCursorPtr)calloc(1, sizeof(rfbCursor));
	unsigned char* Source = (unsigned char*)calloc((size_t)Width * Height, 4);
	unsigned char* Mask = (unsigned char*)calloc(1, (size_t)MaskStride * Height);

	if (!Out || !Source || !Mask)
	{
		free(Out);
		free(Source);
		free(Mask);

		return;
	}

	if (bHaveShape)
	{
		// richSource is in the server's pixel format, which is how Desktop reads the cursor
		// out of the container: 32bpp, blue low.
		memcpy(Source, gShape.data(), (size_t)Width * Height * 4);

		// The alpha byte becomes the transparency mask: RFB's rich cursor has no alpha
		// channel, and which pixels are drawn is a separate 1bpp plane. Bits run from the
		// most significant, one byte per eight pixels, and every row starts a new byte.
		for (unsigned y = 0; y < Height; ++y)
		{
			for (unsigned x = 0; x < Width; ++x)
			{
				if (Source[((size_t)y * Width + x) * 4 + 3])
				{
					Mask[(size_t)y * MaskStride + x / 8] |= (unsigned char)(0x80 >> (x % 8));

					++Opaque;
				}
			}
		}
	}

	Out->width = (unsigned short)Width;
	Out->height = (unsigned short)Height;
	Out->xhot = bHaveShape ? (unsigned short)gHotX : 0;
	Out->yhot = bHaveShape ? (unsigned short)gHotY : 0;
	Out->richSource = Source;
	Out->mask = Mask;
	Out->cleanup = TRUE;
	Out->cleanupRichSource = TRUE;
	Out->cleanupMask = TRUE;

	// Takes ownership of Out and frees what it replaces.
	rfbSetCursor(Screen, Out);

	// Worth printing: a cursor that reaches a viewer and is invisible looks exactly like one
	// that never reached it, and this is the only place that can tell the two apart. Opaque
	// is what the mask actually draws, so zero means an all-transparent bitmap from the
	// container rather than a message that went missing.
	//
	// The two positions are here because a shape that never changes has two causes that
	// look identical from outside: the container's cursor is not moving, or it is moving
	// and something downstream is losing it.
	int PointerX = 0;
	int PointerY = 0;
	Input::PointerPosition(PointerX, PointerY);

	Debug() << "Cursor published: " << Width << "x" << Height
	        << " hotspot " << (bHaveShape ? (int)gHotX : 0)
	        << "," << (bHaveShape ? (int)gHotY : 0)
	        << " opaque pixels " << Opaque
	        << "; container cursor " << Cursor.X << "," << Cursor.Y
	        << ", viewer pointer " << PointerX << "," << PointerY
	        << (Cursor.FromFallback ? " (system arrow standing in)" : "")
	        << (bHaveShape ? "" : " (the container stopped showing one)")
	        << endl;

	gPublished = true;
	gVisible = bHaveShape;
}
