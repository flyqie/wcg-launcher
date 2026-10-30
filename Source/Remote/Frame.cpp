#include "Frame.h"

#include <string.h>

#include <vector>

namespace
{
	std::vector<unsigned char> gFrameBuffer;

	struct Band
	{
		int X1, Y1, X2, Y2;
	};

	const unsigned MaxBands = 32;

	unsigned long long Read64(const unsigned char* Data)
	{
		unsigned long long Value;
		memcpy(&Value, Data, sizeof(Value));
		return Value;
	}
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

int Frame::Update(rfbScreenInfoPtr Screen, const Desktop::Frame& Captured,
	unsigned long long& ChangedArea)
{
	ChangedArea = 0;

	if (Captured.Width != Screen->width || Captured.Height != Screen->height)
	{
		std::vector<unsigned char> Replacement((size_t)Captured.Width * Captured.Height * 4, 0);

		rfbNewFramebuffer(Screen, (char*)Replacement.data(),
			Captured.Width, Captured.Height, 8, 3, 4);

		gFrameBuffer.swap(Replacement);

		ApplyServerFormat(Screen);
	}

	const int Stride = Captured.Stride;
	const unsigned char* Source = Captured.Bits;
	unsigned char* Target = gFrameBuffer.data();

	Band Bands[MaxBands];
	unsigned BandCount = 0;
	bool bTooFragmented = false;

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

		int First = 0;

		while (First + 1 < Captured.Width
			&& Read64(SourceRow + (size_t)First * 4)
				== Read64(TargetRow + (size_t)First * 4))
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
			&& Read64(SourceRow + (size_t)(Last - 1) * 4)
				== Read64(TargetRow + (size_t)(Last - 1) * 4))
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
