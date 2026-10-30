#include "FrameProfile.h"

#include <Windows.h>

#include <algorithm>
#include <iostream>
#include <vector>

#include "Common/LauncherLog.h"
#include "Desktop.h"

namespace
{
	const unsigned Samples = 128;

	const bool gProfiling = DebugEnabled();

	struct Profile
	{
		ULONGLONG Capture[Samples];
		ULONGLONG Dock[Samples];
		ULONGLONG Cursor[Samples];
		ULONGLONG Input[Samples];
		ULONGLONG Diff[Samples];
		unsigned  Calls[Samples];

		ULONGLONG CaptureTotal;
		ULONGLONG DockTotal;
		ULONGLONG CursorTotal;
		ULONGLONG InputTotal;
		ULONGLONG DiffTotal;

		ULONGLONG AllocTotal;
		ULONGLONG FillsTotal;
		ULONGLONG PassTotal;
		ULONGLONG TopPrintTotal;
		ULONGLONG ChildPrintTotal;
		unsigned  TopPrintCalls;
		unsigned  ChildPrintCalls;
		unsigned  ChildRepairs;

		unsigned  Count;
		unsigned  FramesChanged;
		unsigned  CursorMissing;
		unsigned  CursorRead;
		unsigned  Skipped;
		unsigned  Viewers;

		ULONGLONG RectsTotal;
		unsigned  RectsMax;

		ULONGLONG BytesWindow;
		ULONGLONG BytesSeen;

		ULONGLONG ThreadCpu;
		ULONGLONG ProcessCpu;

		ULONGLONG WindowStart;
	};

	Profile gProfile;

	ULONGLONG CpuOf(const FILETIME& Kernel, const FILETIME& User)
	{
		const ULONGLONG K = ((ULONGLONG)Kernel.dwHighDateTime << 32) | Kernel.dwLowDateTime;
		const ULONGLONG U = ((ULONGLONG)User.dwHighDateTime << 32) | User.dwLowDateTime;

		return K + U;
	}

	ULONGLONG ThreadCpu()
	{
		FILETIME Creation, Exit, Kernel, User;

		if (!GetThreadTimes(GetCurrentThread(), &Creation, &Exit, &Kernel, &User))
		{
			return 0;
		}

		return CpuOf(Kernel, User);
	}

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
		     << "  max " << MaxOf(Values, N) << std::endl;
	}

	void Report(ULONGLONG FrameIntervalUs)
	{
		const unsigned N = gProfile.Count;

		if (!N)
		{
			return;
		}

		const ULONGLONG ElapsedMs = GetTickCount64() - gProfile.WindowStart;

		const ULONGLONG Fps = ElapsedMs ? (N * 1000ULL / ElapsedMs) : 0;

		Debug() << "--- frame profile: " << N << " frames in " << ElapsedMs
		     << " ms (" << Fps << " fps), " << gProfile.FramesChanged << " with changes, "
		     << gProfile.Viewers << " viewer(s), "
		     << (gProfile.BytesWindow / 1024) << " kB sent ---" << std::endl;

		if (gProfile.RectsTotal)
		{
			Debug() << "  marked " << (gProfile.RectsTotal / N)
			     << " rectangles per frame, " << gProfile.RectsMax << " at most" << std::endl;
		}

		if (gProfile.Skipped)
		{
			Debug() << "  skipped " << gProfile.Skipped
			     << " frames, nothing connected to send them to" << std::endl;
		}

		{
			const ULONGLONG ThreadUs = (ThreadCpu() - gProfile.ThreadCpu) / 10;
			const ULONGLONG ProcessUs = (ProcessCpu() - gProfile.ProcessCpu) / 10;
			const ULONGLONG ElsewhereUs = ProcessUs > ThreadUs ? ProcessUs - ThreadUs : 0;

			Debug() << "  cpu us/frame  this thread " << (ThreadUs / N)
			     << ", whole process " << (ProcessUs / N)
			     << ", so " << (ElsewhereUs / N)
			     << " elsewhere (the encoder and the sockets)" << std::endl;
		}

		Debug() << "  cursor reads " << N
		     << ", no handle " << gProfile.CursorMissing
		     << ", bitmap read " << gProfile.CursorRead << std::endl;

		Line("capture", gProfile.Capture, gProfile.CaptureTotal, N);
		Line("dock   ", gProfile.Dock, gProfile.DockTotal, N);
		Line("cursor ", gProfile.Cursor, gProfile.CursorTotal, N);
		Line("input  ", gProfile.Input, gProfile.InputTotal, N);
		Line("diff   ", gProfile.Diff, gProfile.DiffTotal, N);

		Debug() << "  PrintWindow calls per frame   max " << MaxOf(gProfile.Calls, N) << std::endl;

		Debug() << "  capture split  us   alloc " << (gProfile.AllocTotal / N)
		     << "  fills " << (gProfile.FillsTotal / N)
		     << "  windows " << (gProfile.PassTotal / N) << std::endl;

		Debug() << "    of which windows   toplevel " << (gProfile.TopPrintTotal / N) << " us";
		if (gProfile.TopPrintCalls)
		{
			Debug() << " x" << gProfile.TopPrintCalls << " => "
			     << (gProfile.TopPrintTotal / gProfile.TopPrintCalls) << " us/call";
		}
		Debug() << std::endl;

		Debug() << "                       child    " << (gProfile.ChildPrintTotal / N) << " us";
		if (gProfile.ChildPrintCalls)
		{
			Debug() << " x" << gProfile.ChildPrintCalls << " => "
			     << (gProfile.ChildPrintTotal / gProfile.ChildPrintCalls) << " us/call";
		}
		Debug() << std::endl;

		if (gProfile.ChildRepairs)
		{
			Debug() << "                       of which " << gProfile.ChildRepairs
			     << " blank prints replaced from the flicker cache" << std::endl;
		}

		const ULONGLONG BudgetUs = FrameIntervalUs;

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
				     << "% of the " << (FrameIntervalUs / 1000) << " ms budget  =>  ~"
				     << (FrameIntervalUs / MeanTotal) << " containers per core" << std::endl;
			}

			if (WorstTotal)
			{
				Debug() << "  worst frame " << WorstTotal << " us (capture " << gProfile.Capture[WorstIndex]
				     << " + dock " << gProfile.Dock[WorstIndex]
				     << " + input " << gProfile.Input[WorstIndex]
				     << " + diff " << gProfile.Diff[WorstIndex] << ") = "
				     << (WorstTotal * 100 / BudgetUs) << "% of budget  =>  ~"
				     << (FrameIntervalUs / WorstTotal) << " containers per core" << std::endl;
			}

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
		gProfile.ChildRepairs = 0;
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

	if (Sample.Skipped)
	{
		++gProfile.Skipped;

		return;
	}

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
	gProfile.ChildRepairs += B.ChildRepairs;

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

void FrameProfile::Flush(ULONGLONG FrameIntervalUs)
{
	if (!gProfiling)
	{
		return;
	}

	if (gProfile.Count >= Samples
		|| (gProfile.Count && GetTickCount64() - gProfile.WindowStart >= 2000))
	{
		Report(FrameIntervalUs);
	}
}
