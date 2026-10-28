#pragma once

#include <Windows.h>

// The per-second frame profile, printed when LAUNCHER_DEBUG is set. Every entry point
// returns immediately when debugging is off. What the numbers mean, and what they were
// measured at, is in Docs/PERFORMANCE.md.
namespace FrameProfile
{
	// One frame's numbers, in microseconds, and all zero when profiling is off.
	//
	// There is no column for encoding or for the transfer: both happen inside
	// libvncserver, on its own threads, and this program never sees them start or finish.
	// The report gives the whole process's CPU beside this thread's instead, so the
	// difference is at least bounded, and reports what actually left the container.
	struct Frame
	{
		// Every field initialised, so that a caller which forgets one gets zero rather
		// than whatever was on the stack. Skipped is the one that matters: a garbage value
		// there would have a real frame counted as one that was not taken.
		ULONGLONG Capture = 0;   // Desktop::Capture and Desktop::Finish together
		ULONGLONG Dock = 0;      // Dock::Draw
		ULONGLONG Cursor = 0;    // Desktop::ReadCursor, and publishing a shape if it changed
		ULONGLONG Input = 0;     // draining what the viewers asked for and injecting it
		ULONGLONG Diff = 0;      // diffing against the framebuffer and copying what changed
		bool      Changed = false;   // a region was marked modified
		unsigned  Rects = 0;     //   how many rectangles that took
		unsigned  Clients = 0;   // viewers connected when the frame was taken

		// Counted over the window because a cursor that never changes shape has several
		// causes that look identical from outside, and these are what tells them apart.
		bool      CursorMissing = false;   // the desktop reported no cursor handle
		bool      CursorRead = false;      // the bitmap was read rather than reused

		// A frame that was not taken, because nothing was connected to send it to.
		// Nothing else in the structure means anything when this is set.
		bool      Skipped = false;

		// Cumulative bytes libvncserver has sent across all viewers, which the report
		// differences over its window.
		unsigned long long BytesSent = 0;
	};

	// Starts the profile's clock and its CPU baselines, before the first frame. Called
	// once, and it has to be: Add runs at the *end* of a frame, so a baseline taken there
	// would cover everything after that frame's work and none of it, and the first window
	// would report a loop that used no CPU at all.
	void Start();

	void Add(const Frame& Sample);

	// Prints and resets, once a full window of samples has built up or two seconds have
	// gone by.
	void Flush(DWORD FrameIntervalMs);
}
