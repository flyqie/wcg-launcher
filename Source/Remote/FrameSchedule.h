#pragma once

#include <Windows.h>

// Capture deadlines are independent of how often input wakes the loop.
namespace FrameSchedule
{
	inline void OnInput(ULONGLONG Now, ULONGLONG LastFrame, DWORD Interval,
		ULONGLONG& NextFrame, unsigned& IdleFrames, unsigned& Backoff)
	{
		IdleFrames = 0;
		Backoff = 1;

		const ULONGLONG Earliest = LastFrame + Interval;
		const ULONGLONG Due = Now > Earliest ? Now : Earliest;
		if (NextFrame > Due) { NextFrame = Due; }
	}
}
