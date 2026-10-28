#include "Frame.h"

#include <string.h>

#include <vector>

using namespace std;

namespace
{
	// The framebuffer libvncserver serves, and the reference the next frame is compared
	// against -- one buffer rather than two, so the comparison and the copy are the same
	// pass over the same bytes.
	//
	// Owned here rather than pointed at Desktop's DIB, which is reused and reallocated
	// when the display mode changes while libvncserver reads this from its own threads.
	//
	// libvncserver also writes into it, for viewers that do not support cursor shape
	// updates: those get the cursor drawn into the picture. That is why the two buffers
	// cannot be assumed identical just because no frame was captured in between.
	vector<unsigned char> gFrameBuffer;

	// One rectangle of changed pixels.
	struct Band
	{
		int X1, Y1, X2, Y2;
	};

	// Bounded: each rectangle costs the viewer a header and at least one tile, so a change
	// fragmented past this is cheaper to send as one box. See Update.
	const unsigned MaxBands = 32;
}

void Frame::ApplyServerFormat(rfbScreenInfoPtr Screen)
{
	Screen->serverFormat.redShift = 16;
	Screen->serverFormat.greenShift = 8;
	Screen->serverFormat.blueShift = 0;
	Screen->serverFormat.redMax = 255;
	Screen->serverFormat.greenMax = 255;
	Screen->serverFormat.blueMax = 255;
}

void Frame::Attach(rfbScreenInfoPtr Screen, int Width, int Height)
{
	gFrameBuffer.assign((size_t)Width * Height * 4, 0);

	Screen->frameBuffer = (char*)gFrameBuffer.data();
}

// Rows first: memcmp over a row is vectorised and settles an unchanged row in one call,
// and an unchanged row is the common case. Only the rows that differ pay for the search
// for the first and last pixel that changed, and only the span between them is copied.
//
// Then rectangles. A single bounding box over every change is the wrong shape for a
// desktop: a caret blinking in a text field and a clock updating in the tray are two
// small changes at opposite corners whose box is the whole screen. Consecutive changed
// rows whose spans overlap become one rectangle instead, so a frame costs what changed
// rather than what it is spread across.
//
// Marking a rectangle wider than the pixels actually copied is safe, and the bands rely
// on it: anything inside a rectangle that did not change is already correct in the
// framebuffer.
int Frame::Update(rfbScreenInfoPtr Screen, const Desktop::Frame& Captured,
	unsigned long long& ChangedArea)
{
	ChangedArea = 0;

	if (Captured.Width != Screen->width || Captured.Height != Screen->height)
	{
		// The display mode changed under us. Hand libvncserver a new framebuffer
		// at the new size and it tells every connected viewer about it.
		gFrameBuffer.assign((size_t)Captured.Width * Captured.Height * 4, 0);

		rfbNewFramebuffer(Screen, (char*)gFrameBuffer.data(),
			Captured.Width, Captured.Height, 8, 3, 4);

		// rfbNewFramebuffer rebuilds the pixel format from the byte count, which
		// undoes the shifts set when the screen was created.
		ApplyServerFormat(Screen);
	}

	const int Stride = Captured.Stride;
	const unsigned char* Source = Captured.Bits;
	unsigned char* Target = gFrameBuffer.data();

	Band Bands[MaxBands];
	unsigned BandCount = 0;
	bool bTooFragmented = false;

	// The bounds of everything that changed, for the case where there are too many
	// rectangles to be worth sending.
	int MinX = Captured.Width;
	int MinY = -1;
	int MaxX = -1;
	int MaxY = -1;

	for (int y = 0; y < Captured.Height; ++y)
	{
		const unsigned char* SourceRow = Source + (size_t)y * Stride;
		unsigned char* TargetRow = Target + (size_t)y * Stride;

		if (memcmp(SourceRow, TargetRow, (size_t)Stride) == 0)
		{
			continue;
		}

		// Where the change starts and stops, two pixels at a time where it can be.
		//
		// A comparison per pixel is a thousand branches for a thousand-pixel row, and
		// a full-screen change is 768 of those rows -- which is the frame where the
		// encoder is expensive too, so it is the one that has to be cheap here. The
		// paired loop stops on anything it cannot pair, and both ends are finished
		// pixel by pixel, so an odd width and a change one pixel wide are both just
		// the slow path rather than a wrong answer.
		int First = 0;

		while (First + 1 < Captured.Width
			&& *(const unsigned long long*)(SourceRow + (size_t)First * 4)
				== *(const unsigned long long*)(TargetRow + (size_t)First * 4))
		{
			First += 2;
		}

		while (First < Captured.Width
			&& memcmp(SourceRow + (size_t)First * 4, TargetRow + (size_t)First * 4, 4) == 0)
		{
			++First;
		}

		int Last = Captured.Width - 1;

		while (Last - 1 > First
			&& *(const unsigned long long*)(SourceRow + (size_t)(Last - 1) * 4)
				== *(const unsigned long long*)(TargetRow + (size_t)(Last - 1) * 4))
		{
			Last -= 2;
		}

		while (Last > First
			&& memcmp(SourceRow + (size_t)Last * 4, TargetRow + (size_t)Last * 4, 4) == 0)
		{
			--Last;
		}

		memcpy(TargetRow + (size_t)First * 4, SourceRow + (size_t)First * 4,
			(size_t)(Last - First + 1) * 4);

		const int X2 = Last + 1;

		if (First < MinX) { MinX = First; }
		if (X2 > MaxX) { MaxX = X2; }
		if (MinY < 0) { MinY = y; }

		MaxY = y + 1;

		if (bTooFragmented)
		{
			continue;
		}

		// The row directly under the previous rectangle, reaching into it: the same
		// change carrying on downwards, so it is the same rectangle.
		if (BandCount
			&& y == Bands[BandCount - 1].Y2
			&& First < Bands[BandCount - 1].X2
			&& X2 > Bands[BandCount - 1].X1)
		{
			Band& Previous = Bands[BandCount - 1];

			Previous.Y2 = y + 1;

			if (First < Previous.X1) { Previous.X1 = First; }
			if (X2 > Previous.X2) { Previous.X2 = X2; }

			continue;
		}

		if (BandCount == MaxBands)
		{
			// Too fragmented to describe as rectangles at all. One box over the lot,
			// which is what this did on every frame before it did this.
			bTooFragmented = true;

			continue;
		}

		Band& New = Bands[BandCount++];

		New.X1 = First;
		New.Y1 = y;
		New.X2 = X2;
		New.Y2 = y + 1;
	}

	if (MinY < 0)
	{
		return 0;
	}

	// Half-open, and libvncserver clamps it: rfbMarkRectAsModified(screen, x1,
	// y1, x2, y2) covers x1 up to but not including x2.
	if (bTooFragmented)
	{
		rfbMarkRectAsModified(Screen, MinX, MinY, MaxX, MaxY);

		ChangedArea = (unsigned long long)(MaxX - MinX) * (MaxY - MinY);

		return 1;
	}

	for (unsigned i = 0; i < BandCount; ++i)
	{
		rfbMarkRectAsModified(Screen, Bands[i].X1, Bands[i].Y1, Bands[i].X2, Bands[i].Y2);

		ChangedArea += (unsigned long long)(Bands[i].X2 - Bands[i].X1)
			* (Bands[i].Y2 - Bands[i].Y1);
	}

	return (int)BandCount;
}
