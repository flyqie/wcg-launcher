#include "Server.h"

#include <stdlib.h>

#include "Clipboard.h"
#include "Cursor.h"
#include "Frame.h"
#include "Handover.h"
#include "Input.h"

#include "../Common/LauncherEnv.h"
#include "../Common/LauncherLog.h"
#include "../Desktop.h"
#include "../Dock.h"
#include "../FrameProfile.h"

using namespace std;

// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is Windows 10 1803+; define it here in
// case the SDK headers predate it.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{
	// --- timing -------------------------------------------------------------

	// Off unless LAUNCHER_DEBUG is set, in which case nothing below reads the clock.
	// Measured, a QueryPerformanceCounter pair plus the conversion costs 31.7 ns.
	const bool gProfiling = DebugEnabled();
	LARGE_INTEGER gFrequency;

	// Starts a timer, or leaves it zero when profiling is off.
	void BeginTimer(LARGE_INTEGER& Start)
	{
		if (gProfiling)
		{
			QueryPerformanceCounter(&Start);
		}
	}

	// Microseconds since BeginTimer, or 0 when profiling is off.
	ULONGLONG MicrosecondsSince(const LARGE_INTEGER& Start)
	{
		if (!gProfiling)
		{
			return 0;
		}

		LARGE_INTEGER Now;
		QueryPerformanceCounter(&Now);

		return (ULONGLONG)((Now.QuadPart - Start.QuadPart) * 1000000LL / gFrequency.QuadPart);
	}

	// --- the server ---------------------------------------------------------

	rfbScreenInfoPtr gScreen = NULL;

	// Read once per frame. Held here rather than local to the loop because the profile
	// samples it after the loop is done with it.
	Desktop::Cursor gCursor;

	// What a viewer calls this session: the container's own name, because a viewer with
	// several of these open has nothing else to tell them apart by.
	//
	// Process-lifetime storage, and it has to be -- the screen keeps the pointer rather
	// than copying the string.
	const char* SessionName()
	{
		static const std::string Name = []
		{
			const char* const Fallback = "Windows Container";

			wchar_t Computer[64] = { 0 };
			DWORD Length = (DWORD)(sizeof(Computer) / sizeof(Computer[0]));

			if (!GetComputerNameW(Computer, &Length) || Length == 0)
			{
				return std::string(Fallback);
			}

			// UTF-8: the viewer is not told the encoding and reads it as bytes.
			const int Bytes = WideCharToMultiByte(CP_UTF8, 0, Computer, (int)Length,
				NULL, 0, NULL, NULL);

			if (Bytes <= 0)
			{
				return std::string(Fallback);
			}

			std::string Utf8((size_t)Bytes, 0);

			WideCharToMultiByte(CP_UTF8, 0, Computer, (int)Length, &Utf8[0], Bytes, NULL, NULL);

			return std::string(Fallback) + " - " + Utf8;
		}();

		return Name.c_str();
	}

	// Brings the RFB server up on the first frame, when the screen size is known.
	bool CreateServer(int Width, int Height, int Port)
	{
		// No arguments: rfbGetScreen reads its own switches out of argv, and this
		// program's command line belongs to the program being launched.
		int Argc = 0;
		char** Argv = NULL;

		rfbScreenInfoPtr Screen = rfbGetScreen(&Argc, Argv, Width, Height, 8, 3, 4);

		if (!Screen)
		{
			Debug() << "rfbGetScreen failed" << endl;

			return false;
		}

		Screen->port = Port;
		Screen->desktopName = SessionName();

		// Several viewers at once, rather than the newest displacing the last.
		Screen->alwaysShared = TRUE;

		// libvncserver wants a NULL-terminated, non-const array of plaintext passwords
		// that outlives the screen, which is what these two statics are for.
		const char* Password = LauncherVncPassword();

		if (Password && *Password)
		{
			static std::string PasswordStore;
			static char* PasswordList[2];

			PasswordStore = Password;
			PasswordList[0] = &PasswordStore[0];
			PasswordList[1] = NULL;

			Screen->authPasswdData = PasswordList;
			Screen->passwordCheck = rfbCheckPasswordByList;

			Debug() << "Viewers must give LAUNCHER_VNC_PW" << endl;
		}

		// Every pointer event as it arrives: the default defers motion that does not
		// change the button mask, which would lag the container's cursor behind the
		// viewer's by the deferral interval.
		Screen->deferPtrUpdateTime = 0;

		Frame::ApplyServerFormat(Screen);
		Frame::Attach(Screen, Width, Height);

		Screen->kbdAddEvent = Input::OnKeyboard;
		Screen->ptrAddEvent = Input::OnPointer;
		Screen->setXCutText = Clipboard::OnCutText;

		// Whatever is on the clipboard now is not news to anyone: a viewer connecting
		// should not have its own clipboard replaced as a side effect.
		Clipboard::Seed();

		rfbInitServer(Screen);

		// A bind failure leaves the socket state at READY rather than SHUTDOWN and shows
		// up only on libvncserver's own log -- but it does leave an invalid listen socket
		// behind, and that is what this checks.
		if (Screen->listenSock == RFB_INVALID_SOCKET)
		{
			Debug() << "RFB server could not listen on port " << Port << endl;

			rfbScreenCleanup(Screen);

			return false;
		}

		// The event loop, on its own threads: one listener, and per client an input and an
		// output thread. This thread keeps the capture loop and only marks regions
		// modified.
		rfbRunEventLoop(Screen, -1, TRUE);

		gScreen = Screen;

		Debug() << "Serving the desktop on port " << Port << endl;

		return true;
	}

	// --- what the profile reports -------------------------------------------

	// Viewers connected at this instant.
	unsigned CountClients()
	{
		if (!gScreen)
		{
			return 0;
		}

		unsigned Count = 0;
		rfbClientIteratorPtr Iterator = rfbGetClientIterator(gScreen);

		while (rfbClientIteratorNext(Iterator))
		{
			++Count;
		}

		rfbReleaseClientIterator(Iterator);

		return Count;
	}

	// Bytes libvncserver has sent since the server came up, across all viewers.
	// Cumulative; the profile reports the difference over its window. Encoding happens on
	// libvncserver's threads, so this is the only part of that side it can account for.
	unsigned long long BytesSent()
	{
		if (!gScreen)
		{
			return 0;
		}

		unsigned long long Total = 0;
		rfbClientIteratorPtr Iterator = rfbGetClientIterator(gScreen);

		while (rfbClientPtr Client = rfbClientIteratorNext(Iterator))
		{
			Total += (unsigned long long)rfbStatGetSentBytes(Client);
		}

		rfbReleaseClientIterator(Iterator);

		return Total;
	}

	// Waits for the frame's slot or for input, whichever comes first. True if input
	// arrived, in which case the caller should go round again rather than take a frame.
	//
	// Sleep() is quantised to the 15.6 ms system tick, which turned a 33 ms budget into
	// 41-46 ms per frame; a high-resolution waitable timer waits for the interval
	// actually asked for. Input only gets to cut the wait short.
	bool WaitForFrameOrInput(HANDLE hFrameTimer, DWORD Milliseconds)
	{
		const HANDLE hInput = Handover::Event();

		if (!hInput)
		{
			// CreateEvent failed, which it should not. Frames are still paced; input is
			// applied a frame late.
			Sleep(Milliseconds);

			return false;
		}

		if (!hFrameTimer)
		{
			return WaitForSingleObject(hInput, Milliseconds) == WAIT_OBJECT_0;
		}

		// Negative means relative, in 100 ns units.
		LARGE_INTEGER Due;
		Due.QuadPart = -(LONGLONG)Milliseconds * 10000;

		if (!SetWaitableTimer(hFrameTimer, &Due, 0, NULL, NULL, FALSE))
		{
			return WaitForSingleObject(hInput, Milliseconds) == WAIT_OBJECT_0;
		}

		HANDLE Handles[2] = { hFrameTimer, hInput };

		// Bounded so a timer that never fires cannot wedge the loop.
		const DWORD Woken = WaitForMultipleObjects(2, Handles, FALSE, Milliseconds + 50);

		if (Woken == WAIT_OBJECT_0 + 1)
		{
			// Cleared here rather than after the drain: an event arriving in between sets
			// it again and costs one pass that finds nothing, where clearing it afterwards
			// would lose that input until the next frame.
			ResetEvent(hInput);

			return true;
		}

		return false;
	}
}

bool Server::Run()
{
	const bool bServe = LauncherVnc();
	const int Port = LauncherVncPort();

	// Capture rate, overridden by LAUNCHER_FPS. PrintWindow is a synchronous WM_PRINT
	// that forces a repaint of every window, so an uncapped loop starves controls. The
	// cost is roughly fixed per frame, so this trades container CPU for smoothness:
	// 30 fps costs about 30% of a core, 15 fps about half. See Docs/PERFORMANCE.md.
	DWORD FrameIntervalMs = 33;   // ~30 fps

	{
		char Value[16] = { 0 };

		if (GetEnvironmentVariableA("LAUNCHER_FPS", Value, sizeof(Value)))
		{
			const int Fps = atoi(Value);

			if (Fps >= 1 && Fps <= 240)
			{
				FrameIntervalMs = 1000 / (DWORD)Fps;
			}
		}
	}

	if (gProfiling)
	{
		QueryPerformanceFrequency(&gFrequency);
	}

	// Frame pacing; see WaitForFrameOrInput for why it is not Sleep().
	HANDLE hFrameTimer = CreateWaitableTimerExW(
		NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

	if (!hFrameTimer)
	{
		Debug() << "High-resolution timer unavailable (err " << hex << GetLastError()
		     << "), falling back to Sleep" << endl;
	}

	// What wakes the loop when a viewer does something. Manual-reset, so that input
	// arriving while the previous batch is being handled is not lost.
	if (!Handover::Open())
	{
		Debug() << "Input event could not be created (err " << hex << GetLastError()
		     << "), input will be applied a frame late" << endl;
	}

	// When the next frame falls due. Input is applied whenever it arrives, without one.
	ULONGLONG NextFrame = GetTickCount64();

	// How still the screen has to be, and for how long, before the capture rate backs
	// off. See the note in the loop.
	const unsigned IdleFramesBeforeBackoff = 4;
	const unsigned MaxBackoff = 8;      // at most 1/8 of the rate LAUNCHER_FPS asks for

	unsigned IdleFrames = 0;
	unsigned Backoff = 1;

	// How long the desktop may be empty before the session gives up on it. A window is
	// created before it is shown, so one empty sample is not proof the program is gone,
	// and acting on it would end the session before anything had been drawn.
	const ULONGLONG EmptyGraceMs = 2000;

	ULONGLONG EmptySince = 0;

	// Before the loop, so the first window covers the first frame's work.
	FrameProfile::Start();

	bool bContinue = true;

	while (bContinue)
	{
		const ULONGLONG Now = GetTickCount64();

		if (Desktop::HasVisibleWindows())
		{
			EmptySince = 0;
		}
		else if (!EmptySince)
		{
			EmptySince = Now;
		}
		else if (Now - EmptySince >= EmptyGraceMs)
		{
			// Nothing left on the desktop to show.
			Debug() << "No more visible windows, exiting." << endl;
			bContinue = false;
		}

		if (!bServe)
		{
			// LAUNCHER_VNC unset: no server, and the capture is the whole cost of this
			// loop, so no frame is worth taking. The desktop and the program on it are
			// still started; this just waits for that program to finish.
			Sleep(FrameIntervalMs);

			NextFrame = GetTickCount64();

			continue;
		}

		// Whatever the viewers asked for. Here and not in the callbacks that received it,
		// because this thread is the one on the container's desktop; see Handover.
		//
		// At the top of the loop rather than beside the capture, so that input lands the
		// moment it arrives: the wait below returns early for it and comes straight back
		// here without a frame being taken, where taking one would make the capture rate
		// follow the mouse. Before the capture, so that what is applied is painted into
		// the frame about to be sent rather than the one after it.
		//
		// The screen is needed by the dock hit test inside, and nothing can have been
		// asked for before there is a server to connect to anyway.
		LARGE_INTEGER InputStart;
		BeginTimer(InputStart);

		if (gScreen)
		{
			Handover::Apply(gScreen);
		}

		const ULONGLONG InputUs = MicrosecondsSince(InputStart);

		const unsigned Viewers = CountClients();

		Clipboard::Publish(gScreen, Viewers);

		// Nobody is watching, so the frame is not taken -- the capture is the whole cost
		// of the loop. The exception is before the server exists at all, because the
		// screen size is only ever learned from a frame. The desktop emptying is still
		// watched for above, so a session with no viewer still ends when its program does.
		if (gScreen && !Viewers)
		{
			// A viewer connecting is noticed on the next slot at worst, and the first frame
			// it gets is a full one: the framebuffer still holds what was captured before
			// it connected, so the comparison finds every difference. Counted, so that the
			// report does not look like the loop had stopped.
			FrameProfile::Frame Skipped = {};
			Skipped.Skipped = true;

			FrameProfile::Add(Skipped);
			FrameProfile::Flush(FrameIntervalMs);

			Sleep(FrameIntervalMs);

			NextFrame = GetTickCount64();

			continue;
		}

		// Wait for this frame's slot, unless it has already gone by.
		{
			const ULONGLONG Before = GetTickCount64();

			if (Before < NextFrame && WaitForFrameOrInput(hFrameTimer, (DWORD)(NextFrame - Before)))
			{
				// Input, not the clock. Unless the frame has fallen due meanwhile: a viewer
				// who keeps the mouse moving would otherwise keep setting the event and the
				// loop would go round draining without ever reaching the capture. Input gets
				// to cut the wait short, not to postpone the frame.
				if (GetTickCount64() < NextFrame)
				{
					continue;
				}
			}
		}

		// The dock draws itself onto the frame between Capture and Finish, which is the
		// only window in which a Frame is valid.
		Desktop::Frame Captured;

		LARGE_INTEGER CaptureStart;
		BeginTimer(CaptureStart);

		const bool bCaptured = Desktop::Capture(Captured);

		ULONGLONG CaptureUs = MicrosecondsSince(CaptureStart);

		LARGE_INTEGER DockStart;
		BeginTimer(DockStart);

		if (bCaptured)
		{
			Dock::Draw(Captured.Dc, Captured.Width, Captured.Height);
		}

		const ULONGLONG DockUs = MicrosecondsSince(DockStart);

		LARGE_INTEGER FinishStart;
		BeginTimer(FinishStart);

		if (bCaptured)
		{
			Desktop::Finish(Captured);
		}

		CaptureUs += MicrosecondsSince(FinishStart);

		// Before the cursor, and that ordering is load-bearing. ReadCursor reads the bitmap
		// only when the handle changes, which on a fresh session is this frame and never
		// again; reading it while there is no server to publish it to loses it for good,
		// and the viewer is sent an empty cursor instead.
		if (bCaptured && !gScreen && !CreateServer(Captured.Width, Captured.Height, Port))
		{
			if (hFrameTimer)
			{
				CloseHandle(hFrameTimer);
			}

			Handover::Close();

			return false;
		}

		// The cursor is read, not painted into the frame; RFB carries it on its own.
		LARGE_INTEGER CursorStart;
		BeginTimer(CursorStart);

		Desktop::ReadCursor(gCursor);

		if (gScreen)
		{
			Cursor::Publish(gScreen, gCursor);
		}

		const ULONGLONG CursorUs = MicrosecondsSince(CursorStart);

		int Rects = 0;
		unsigned long long ChangedArea = 0;
		ULONGLONG DiffUs = 0;

		if (bCaptured && gScreen)
		{
			LARGE_INTEGER DiffStart;
			BeginTimer(DiffStart);

			Rects = Frame::Update(gScreen, Captured, ChangedArea);

			DiffUs = MicrosecondsSince(DiffStart);
		}

		// Frames with nothing in them are sampled too: they still pay for a full capture.
		FrameProfile::Frame Sample;
		Sample.Capture = CaptureUs;
		Sample.Dock = DockUs;
		Sample.Cursor = CursorUs;
		Sample.CursorMissing = gCursor.HandleMissing;
		Sample.CursorRead = gCursor.ShapeRead;
		Sample.Input = InputUs;
		Sample.Diff = DiffUs;
		Sample.Changed = Rects > 0;
		Sample.Rects = (unsigned)Rects;
		Sample.Clients = Viewers;
		Sample.BytesSent = BytesSent();

		FrameProfile::Add(Sample);
		FrameProfile::Flush(FrameIntervalMs);

		// A frame in which almost nothing moved does not need the next one at full rate,
		// and this is the only lever left: PrintWindow is three quarters of a frame and
		// nothing about it is ours to make cheaper. Docs/PERFORMANCE.md sections 3 and 4
		// record what was tried and measured dead.
		//
		// The interval doubles while the screen is nearly still, up to four doublings, and
		// returns to one the moment anything moves. LAUNCHER_FPS stays the ceiling.
		//
		// Interactivity is unaffected: input wakes this loop directly, so a click is applied
		// and a frame taken at once whatever the back-off had reached. What is delayed is a
		// change made inside the container with nobody touching it -- a clock, a progress
		// bar -- and only until the next look. The rate the profile prints is the rate
		// actually achieved.
		{
			const unsigned long long Still =
				(unsigned long long)Captured.Width * Captured.Height / 100;

			if (ChangedArea <= Still)
			{
				if (++IdleFrames >= IdleFramesBeforeBackoff && Backoff < MaxBackoff)
				{
					Backoff *= 2;
					IdleFrames = 0;
				}
			}
			else
			{
				IdleFrames = 0;
				Backoff = 1;
			}
		}

		// One interval after the frame that was due, not one after this frame finished.
		// Pacing from the end makes the period "the work plus the interval", so a 30 fps
		// setting on a 6 ms frame delivers 25 -- a gap control rather than a rate one.
		NextFrame += FrameIntervalMs * Backoff;

		// A frame that overran its slot, or a container that was suspended, leaves the
		// schedule in the past. Resynchronised rather than made up: a burst to catch up
		// would spend the CPU this setting exists to bound.
		{
			const ULONGLONG After = GetTickCount64();

			if (NextFrame < After)
			{
				NextFrame = After;
			}
		}
	}

	if (hFrameTimer)
	{
		CloseHandle(hFrameTimer);
	}

	Handover::Close();

	if (gScreen)
	{
		// Closing the sockets is what makes the background threads notice, and on Windows
		// that is all rfbShutdownServer does -- the join at the end of it is the pthread
		// path. The listener notices on its next select timeout, 5 ms away, and until then
		// it is still inside rfbProcessEvents holding the screen, so it is waited for
		// rather than guessed at: the next thing that happens is freeing that screen.
		//
		// The per-client threads need no such care; they exit when their socket closes.
		rfbShutdownServer(gScreen, TRUE);

#if defined(LIBVNCSERVER_HAVE_WIN32THREADS)
		if (gScreen->listener_thread)
		{
			// _beginthread hands back a thread handle, which is what this holds.
			WaitForSingleObject((HANDLE)gScreen->listener_thread, 2000);
		}
#endif

		rfbScreenCleanup(gScreen);
		gScreen = NULL;
	}

	return true;
}
