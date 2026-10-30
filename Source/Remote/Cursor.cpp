#include "Cursor.h"

#include <iostream>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "../Common/LauncherLog.h"
#include "Input.h"

namespace
{
	std::vector<unsigned char> gShape;
	unsigned gWidth = 0;
	unsigned gHeight = 0;
	unsigned gHotX = 0;
	unsigned gHotY = 0;

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

	if (gShape.empty())
	{
		return;
	}

	const bool bHaveShape = Cursor.Visible;
	const bool bNewShape = Cursor.ShapeChanged && !Cursor.Shape.empty();

	if (gPublished && !bNewShape && bHaveShape == gVisible)
	{
		return;
	}

	const unsigned Width = bHaveShape ? gWidth : 1;
	const unsigned Height = bHaveShape ? gHeight : 1;

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
		memcpy(Source, gShape.data(), (size_t)Width * Height * 4);

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

	rfbSetCursor(Screen, Out);

	int PointerX = 0;
	int PointerY = 0;
	Input::PointerPosition(PointerX, PointerY);

	Debug() << "Cursor published: " << Width << "x" << Height
	        << " hotspot " << (bHaveShape ? (int)gHotX : 0)
	        << "," << (bHaveShape ? (int)gHotY : 0)
	        << " opaque pixels " << Opaque
	        << "; container cursor " << Cursor.X << "," << Cursor.Y
	        << ", viewer pointer " << PointerX << "," << PointerY
	        << (Cursor.FromWindowClass ? " (the window under the pointer's class cursor)"
	            : Cursor.FromFallback ? " (system arrow standing in)" : "")
	        << (bHaveShape ? "" : " (the container stopped showing one)")
	        << std::endl;

	gPublished = true;
	gVisible = bHaveShape;
}
