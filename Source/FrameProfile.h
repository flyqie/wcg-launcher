#pragma once

#include <Windows.h>

namespace FrameProfile
{
	struct Frame
	{
		ULONGLONG Capture = 0;
		ULONGLONG Dock = 0;
		ULONGLONG Cursor = 0;
		ULONGLONG Input = 0;
		ULONGLONG Diff = 0;
		bool      Changed = false;
		unsigned  Rects = 0;
		unsigned  Clients = 0;

		bool      CursorMissing = false;
		bool      CursorRead = false;

		bool      Skipped = false;

		unsigned long long BytesSent = 0;
	};

	void Start();

	void Add(const Frame& Sample);

	void Flush(ULONGLONG FrameIntervalUs);
}
