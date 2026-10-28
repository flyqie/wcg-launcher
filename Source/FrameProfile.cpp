#include "FrameProfile.h"

#include <Windows.h>

#include <algorithm>
#include <iostream>
#include <vector>

#include "Common/LauncherLog.h"
#include "Desktop.h"

using namespace std;

namespace
{
	// How many frames a report covers before it is flushed on sample count alone.
	const unsigned Samples = 128;

	// Off unless LAUNCHER_DEBUG is set: the report is worth nothing printed into a
	// discarded stream.
	const bool gProfiling = DebugEnabled();

	struct Profile
	{
		ULONGLONG Capture[Samples];
		ULONGLONG Dock[Samples];
		ULONGLONG Cursor[Samples];
		ULONGLONG Input[Samples];
		ULONGLONG Diff[Samples];
		unsigned  Calls[Samples];   // PrintWindow calls made by that frame

		ULONGLONG CaptureTotal;
		ULONGLONG DockTotal;
		ULONGLONG CursorTotal;
		ULONGLONG InputTotal;
		ULONGLONG DiffTotal;

		// The capture's own breakdown, accumulated so the report can show means.
		// See Desktop::CaptureBreakdown.
		ULONGLONG AllocTotal;
		ULONGLONG FillsTotal;
		ULONGLONG PassTotal;
		ULONGLONG TopPrintTotal;
		ULONGLONG ChildPrintTotal;
		unsigned  TopPrintCalls;
		unsigned  ChildPrintCalls;

		unsigned  Count;
		unsigned  FramesChanged;   // frames in which a region was marked modified
		unsigned  CursorMissing;   // frames in which the desktop reported no cursor handle
		unsigned  CursorRead;      // frames in which the cursor bitmap was actually read
		unsigned  Skipped;         // frames not taken, because nothing was connected
		unsigned  Viewers;         // the most seen at once during the window

		// Rectangles marked, which is what the frame costs the encoder and the link.
		// A single bounding box over everything -- what this used to mark -- is one
		// rectangle however far apart the changes are; see UpdateFrameBuffer.
		ULONGLONG RectsTotal;
		unsigned  RectsMax;

		// What actually left the container, in bytes, over this window. Accumulated
		// rather than differenced: libvncserver's counters are per client and are reset
		// when one goes away, so a plain difference would underflow. See Add.
		ULONGLONG BytesWindow;
		ULONGLONG BytesSeen;       // the last total observed, never reset

		// CPU time, in 100 ns units, at the start of the window: this thread's and the
		// whole process's. The difference between the two is everything that is not
		// this loop -- libvncserver's encoder, its sockets and its threads.
		ULONGLONG ThreadCpu;
		ULONGLONG ProcessCpu;

		ULONGLONG WindowStart;
	};

	Profile gProfile;

	// CPU time a set of times describes, in 100 ns units.
	ULONGLONG CpuOf(const FILETIME& Kernel, const FILETIME& User)
	{
		const ULONGLONG K = ((ULONGLONG)Kernel.dwHighDateTime << 32) | Kernel.dwLowDateTime;
		const ULONGLONG U = ((ULONGLONG)User.dwHighDateTime << 32) | User.dwLowDateTime;

		return K + U;
	}

	// CPU time this thread has used. Called from the frame loop, so "this thread" is
	// the one the profile is about.
	ULONGLONG ThreadCpu()
	{
		FILETIME Creation, Exit, Kernel, User;

		if (!GetThreadTimes(GetCurrentThread(), &Creation, &Exit, &Kernel, &User))
		{
			return 0;
		}

		return CpuOf(Kernel, User);
	}

	// CPU time every thread in this process has used between them, which is the only
	// way to see what libvncserver's threads are spending.
	ULONGLONG ProcessCpu()
	{
		FILETIME Creation, Exit, Kernel, User;

		if (!GetProcessTimes(GetCurrentProcess(), &Creation, &Exit, &Kernel, &User))
		{
			return 0;
		}

		return CpuOf(Kernel, User);
	}

	ULONGLONG Percentile(const ULONGLONG* Values, unsigned Count, unsigned Pct)
	{
		if (!Count)
		{
			return 0;
		}

		std::vector<ULONGLONG> Sorted(Values, Values + Count);
		std::sort(Sorted.begin(), Sorted.end());

		return Sorted[(size_t)Pct * (Count - 1) / 100];
	}

	ULONGLONG MaxOf(const ULONGLONG* Values, unsigned Count)
	{
		ULONGLONG Max = 0;

		for (unsigned i = 0; i < Count; ++i)
		{
			if (Values[i] > Max)
			{
				Max = Values[i];
			}
		}

		return Max;
	}

	unsigned MaxOf(const unsigned* Values, unsigned Count)
	{
		unsigned Max = 0;

		for (unsigned i = 0; i < Count; ++i)
		{
			if (Values[i] > Max)
			{
				Max = Values[i];
			}
		}

		return Max;
	}

	void Line(const char* Name, const ULONGLONG* Values, ULONGLONG Total, unsigned N)
	{
		Debug() << "  " << Name << " us   mean " << (Total / N)
		     << "  p50 " << Percentile(Values, N, 50)
		     << "  p95 " << Percentile(Values, N, 95)
		     << "  max " << MaxOf(Values, N) << endl;
	}

	void Report(DWORD FrameIntervalMs)
	{
		const unsigned N = gProfile.Count;

		if (!N)
		{
			return;
		}

		const ULONGLONG ElapsedMs = GetTickCount64() - gProfile.WindowStart;

		// The rate is here because LAUNCHER_FPS is the knob for CPU and for bandwidth,
		// and a knob is worth nothing if the thing it turns cannot be read off.
		const ULONGLONG Fps = ElapsedMs ? (N * 1000ULL / ElapsedMs) : 0;

		Debug() << "--- frame profile: " << N << " frames in " << ElapsedMs
		     << " ms (" << Fps << " fps), " << gProfile.FramesChanged << " with changes, "
		     << gProfile.Viewers << " viewer(s), "
		     << (gProfile.BytesWindow / 1024) << " kB sent ---" << endl;

		if (gProfile.RectsTotal)
		{
			// The number to watch for the transmission: what changed, rather than where.
			Debug() << "  marked " << (gProfile.RectsTotal / N)
			     << " rectangles per frame, " << gProfile.RectsMax << " at most" << endl;
		}

		if (gProfile.Skipped)
		{
			// Worth saying out loud: the window covered more wall time than N frames'
			// worth of it, so the shares below are of the frames that were taken.
			Debug() << "  skipped " << gProfile.Skipped
			     << " frames, nothing connected to send them to" << endl;
		}

		// What this loop costs, and what the rest of the process costs on top of it. The
		// encoder, the sockets and their threads are libvncserver's and cannot be timed
		// from here, but the process's own CPU time counts them all, so the gap between
		// the two lines is what they are spending. See Docs/PERFORMANCE.md, section 5.
		{
			const ULONGLONG ThreadUs = (ThreadCpu() - gProfile.ThreadCpu) / 10;
			const ULONGLONG ProcessUs = (ProcessCpu() - gProfile.ProcessCpu) / 10;
			const ULONGLONG ElsewhereUs = ProcessUs > ThreadUs ? ProcessUs - ThreadUs : 0;

			Debug() << "  cpu us/frame  this thread " << (ThreadUs / N)
			     << ", whole process " << (ProcessUs / N)
			     << ", so " << (ElsewhereUs / N)
			     << " elsewhere (the encoder and the sockets)" << endl;
		}

		// A cursor that will not change shape: the desktop reporting no handle at all
		// is a different fault from it reporting one whose bitmap is the same, and
		// the two are indistinguishable from the viewer's side.
		Debug() << "  cursor reads " << N
		     << ", no handle " << gProfile.CursorMissing
		     << ", bitmap read " << gProfile.CursorRead << endl;

		Line("capture", gProfile.Capture, gProfile.CaptureTotal, N);
		Line("dock   ", gProfile.Dock, gProfile.DockTotal, N);
		Line("cursor ", gProfile.Cursor, gProfile.CursorTotal, N);
		Line("input  ", gProfile.Input, gProfile.InputTotal, N);
		Line("diff   ", gProfile.Diff, gProfile.DiffTotal, N);

		Debug() << "  PrintWindow calls per frame   max " << MaxOf(gProfile.Calls, N) << endl;

		// The part worth optimising: it does not shrink with better encoding.
		Debug() << "  capture split  us   alloc " << (gProfile.AllocTotal / N)
		     << "  fills " << (gProfile.FillsTotal / N)
		     << "  windows " << (gProfile.PassTotal / N) << endl;

		// Per-call costs, because a top-level window and a child button differ by
		// an order of magnitude.
		Debug() << "    of which windows   toplevel " << (gProfile.TopPrintTotal / N) << " us";
		if (gProfile.TopPrintCalls)
		{
			Debug() << " x" << gProfile.TopPrintCalls << " => "
			     << (gProfile.TopPrintTotal / gProfile.TopPrintCalls) << " us/call";
		}
		Debug() << endl;

		Debug() << "                       child    " << (gProfile.ChildPrintTotal / N) << " us";
		if (gProfile.ChildPrintCalls)
		{
			Debug() << " x" << gProfile.ChildPrintCalls << " => "
			     << (gProfile.ChildPrintTotal / gProfile.ChildPrintCalls) << " us/call";
		}
		Debug() << endl;

		// The capacity answer for this side of the work: a frame's cost as a share
		// of the pacing interval is the fraction of one core the capture loop needs,
		// and its inverse is how many fit. It is a floor rather than the whole
		// answer, because libvncserver encodes and sends on its own threads and none
		// of that time is in here.
		const ULONGLONG BudgetUs = (ULONGLONG)FrameIntervalMs * 1000;

		if (BudgetUs)
		{
			ULONGLONG WorstTotal = 0;
			unsigned WorstIndex = 0;

			for (unsigned i = 0; i < N; ++i)
			{
				const ULONGLONG Total = gProfile.Capture[i] + gProfile.Dock[i]
					+ gProfile.Input[i] + gProfile.Diff[i];

				if (Total > WorstTotal)
				{
					WorstTotal = Total;
					WorstIndex = i;
				}
			}

			const ULONGLONG MeanTotal = (gProfile.CaptureTotal + gProfile.DockTotal
				+ gProfile.InputTotal + gProfile.DiffTotal) / N;

			if (MeanTotal)
			{
				Debug() << "  mean frame " << MeanTotal << " us = " << (MeanTotal * 100 / BudgetUs)
				     << "% of the " << FrameIntervalMs << " ms budget  =>  ~"
				     << (1000ULL * FrameIntervalMs / MeanTotal) << " containers per core" << endl;
			}

			if (WorstTotal)
			{
				Debug() << "  worst frame " << WorstTotal << " us (capture " << gProfile.Capture[WorstIndex]
				     << " + dock " << gProfile.Dock[WorstIndex]
				     << " + input " << gProfile.Input[WorstIndex]
				     << " + diff " << gProfile.Diff[WorstIndex] << ") = "
				     << (WorstTotal * 100 / BudgetUs) << "% of budget  =>  ~"
				     << (1000ULL * FrameIntervalMs / WorstTotal) << " containers per core" << endl;
			}

			Debug() << "  capture side only; the encoder and the sockets are libvncserver's threads" << endl;
		}

		gProfile.Count = 0;
		gProfile.FramesChanged = 0;
		gProfile.CursorMissing = 0;
		gProfile.CursorRead = 0;
		gProfile.Skipped = 0;
		gProfile.Viewers = 0;
		gProfile.RectsTotal = 0;
		gProfile.RectsMax = 0;
		gProfile.BytesWindow = 0;
		gProfile.CaptureTotal = 0;
		gProfile.DockTotal = 0;
		gProfile.CursorTotal = 0;
		gProfile.InputTotal = 0;
		gProfile.DiffTotal = 0;
		gProfile.AllocTotal = 0;
		gProfile.FillsTotal = 0;
		gProfile.PassTotal = 0;
		gProfile.TopPrintTotal = 0;
		gProfile.ChildPrintTotal = 0;
		gProfile.TopPrintCalls = 0;
		gProfile.ChildPrintCalls = 0;
		gProfile.ThreadCpu = ThreadCpu();
		gProfile.ProcessCpu = ProcessCpu();
		gProfile.WindowStart = GetTickCount64();
	}
}

void FrameProfile::Start()
{
	if (!gProfiling)
	{
		return;
	}

	gProfile.Count = 0;
	gProfile.Skipped = 0;
	gProfile.BytesSeen = 0;
	gProfile.BytesWindow = 0;
	gProfile.ThreadCpu = ThreadCpu();
	gProfile.ProcessCpu = ProcessCpu();
	gProfile.WindowStart = GetTickCount64();
}

void FrameProfile::Add(const Frame& Sample)
{
	if (!gProfiling)
	{
		return;
	}

	// A frame that was not taken is not a sample. It has no cost to report, and
	// averaging it in would make every mean below describe a loop that is not the one
	// running -- so it is counted and nothing else.
	if (Sample.Skipped)
	{
		++gProfile.Skipped;

		return;
	}

	// Bytes libvncserver has sent since the last sample, if that went up.
	//
	// The counters behind this are per client and reset when one goes away, so the sum
	// falls whenever a viewer leaves -- which a plain "end minus start" reads as an
	// enormous number, having gone below zero in an unsigned type. Counting only the
	// rises keeps it monotonic.
	//
	// Before the early return below, so a full window still counts the frame that
	// filled it.
	if (Sample.BytesSent > gProfile.BytesSeen)
	{
		gProfile.BytesWindow += Sample.BytesSent - gProfile.BytesSeen;
	}

	gProfile.BytesSeen = Sample.BytesSent;

	if (gProfile.Count >= Samples)
	{
		return;
	}

	const unsigned i = gProfile.Count;

	gProfile.Capture[i] = Sample.Capture;
	gProfile.Dock[i] = Sample.Dock;
	gProfile.Cursor[i] = Sample.Cursor;
	gProfile.Input[i] = Sample.Input;
	gProfile.Diff[i] = Sample.Diff;
	gProfile.Calls[i] = Desktop::LastPrintWindowCalls();

	gProfile.CaptureTotal += Sample.Capture;
	gProfile.DockTotal += Sample.Dock;
	gProfile.CursorTotal += Sample.Cursor;
	gProfile.InputTotal += Sample.Input;
	gProfile.DiffTotal += Sample.Diff;

	const Desktop::CaptureBreakdown& B = Desktop::LastCaptureBreakdown();
	gProfile.AllocTotal += B.BitmapAlloc;
	gProfile.FillsTotal += B.Fills;
	gProfile.PassTotal += B.WindowPass;
	gProfile.TopPrintTotal += B.TopLevelPrint;
	gProfile.ChildPrintTotal += B.ChildReprint;
	gProfile.TopPrintCalls += B.TopLevelCalls;
	gProfile.ChildPrintCalls += B.ChildCalls;

	if (Sample.Clients > gProfile.Viewers)
	{
		gProfile.Viewers = Sample.Clients;
	}

	++gProfile.Count;

	if (Sample.Changed)
	{
		++gProfile.FramesChanged;
	}

	gProfile.RectsTotal += Sample.Rects;

	if (Sample.Rects > gProfile.RectsMax)
	{
		gProfile.RectsMax = Sample.Rects;
	}

	if (Sample.CursorMissing)
	{
		++gProfile.CursorMissing;
	}

	if (Sample.CursorRead)
	{
		++gProfile.CursorRead;
	}
}

void FrameProfile::Flush(DWORD FrameIntervalMs)
{
	if (!gProfiling)
	{
		return;
	}

	// On a full window of samples, or on time if frames are slow enough that a
	// full window would take too long to fill.
	if (gProfile.Count >= Samples
		|| (gProfile.Count && GetTickCount64() - gProfile.WindowStart >= 2000))
	{
		Report(FrameIntervalMs);
	}
}
