#include "Server.h"

#include <errno.h>
#include <stdlib.h>
#include <vector>

#include "Clipboard.h"
#include "Cursor.h"
#include "Frame.h"
#include "FrameSchedule.h"
#include "Handover.h"
#include "Input.h"

#include "../Common/LauncherEnv.h"
#include "../Common/LauncherLog.h"
#include "../Desktop.h"
#include "../Dock.h"
#include "../FrameProfile.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{

	const bool gProfiling = DebugEnabled();
	LARGE_INTEGER gFrequency;

	void BeginTimer(LARGE_INTEGER& Start)
	{
		if (gProfiling)
		{
			QueryPerformanceCounter(&Start);
		}
	}

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

	rfbScreenInfoPtr gScreen = NULL;

	Desktop::Cursor gCursor;

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

	bool CreateServer(int Width, int Height, int Port)
	{
		int Argc = 0;
		char** Argv = NULL;

		rfbScreenInfoPtr Screen = rfbGetScreen(&Argc, Argv, Width, Height, 8, 3, 4);

		if (!Screen)
		{
			Debug() << "rfbGetScreen failed" << std::endl;

			return false;
		}

		Screen->port = Port;
		Screen->desktopName = SessionName();

		Screen->alwaysShared = TRUE;

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

			Debug() << "Viewers must give LAUNCHER_VNC_PW" << std::endl;
		}

		Screen->deferPtrUpdateTime = 0;

		Frame::ApplyServerFormat(Screen);
		Frame::Attach(Screen, Width, Height);

		Screen->kbdAddEvent = Input::OnKeyboard;
		Screen->ptrAddEvent = Input::OnPointer;
		Screen->setXCutText = Clipboard::OnCutText;

		Screen->setXCutTextUTF8 = Clipboard::OnCutTextUtf8;

		Clipboard::Seed();

		rfbInitServer(Screen);

		if (Screen->listenSock == RFB_INVALID_SOCKET)
		{
			Debug() << "RFB server could not listen on port " << Port << std::endl;

			rfbScreenCleanup(Screen);

			return false;
		}

		rfbRunEventLoop(Screen, -1, TRUE);

		gScreen = Screen;

		Debug() << "Serving the desktop on port " << Port << std::endl;

		return true;
	}

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

#if defined(LIBVNCSERVER_HAVE_WIN32THREADS)
	bool CloseClientConnections(rfbScreenInfoPtr Screen)
	{
		struct PendingClient
		{
			HANDLE Thread;
		};

		std::vector<PendingClient> Pending;
		rfbClientIteratorPtr Iterator = rfbGetClientIterator(Screen);

		while (rfbClientPtr Client = rfbClientIteratorNext(Iterator))
		{
			PendingClient Stop = { NULL };

			if (Client->client_thread
				&& Client->client_thread != (uintptr_t)-1
				&& DuplicateHandle(GetCurrentProcess(), (HANDLE)Client->client_thread,
					GetCurrentProcess(), &Stop.Thread, SYNCHRONIZE, FALSE, 0))
			{
			}

			rfbCloseClient(Client);
			if (Client->sock != RFB_INVALID_SOCKET)
			{
				shutdown(Client->sock, SD_BOTH);
			}
			Pending.push_back(Stop);
		}

		rfbReleaseClientIterator(Iterator);

		bool AllStopped = true;

		for (const PendingClient& Stop : Pending)
		{
			if (Stop.Thread)
			{
				if (WaitForSingleObject(Stop.Thread, 2000) != WAIT_OBJECT_0)
				{
					AllStopped = false;
				}

				CloseHandle(Stop.Thread);
			}
			else
			{
				AllStopped = false;
			}
		}

		return AllStopped;
	}
#endif

	bool WaitForFrameOrInput(HANDLE hFrameTimer, DWORD Milliseconds)
	{
		const HANDLE hInput = Handover::Event();

		if (!hInput)
		{
			Sleep(Milliseconds);

			return false;
		}

		if (!hFrameTimer)
		{
			return WaitForSingleObject(hInput, Milliseconds) == WAIT_OBJECT_0;
		}

		LARGE_INTEGER Due;
		Due.QuadPart = -(LONGLONG)Milliseconds * 10000;

		if (!SetWaitableTimer(hFrameTimer, &Due, 0, NULL, NULL, FALSE))
		{
			return WaitForSingleObject(hInput, Milliseconds) == WAIT_OBJECT_0;
		}

		HANDLE Handles[2] = { hFrameTimer, hInput };

		const DWORD Woken = WaitForMultipleObjects(2, Handles, FALSE, Milliseconds + 50);

		if (Woken == WAIT_OBJECT_0 + 1)
		{
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

	const ULONGLONG FrameIntervalUs = 1000000ULL / (ULONGLONG)LauncherFps();

	ULONGLONG CarryUs = 0;

	const DWORD FrameIntervalMs = (DWORD)(FrameIntervalUs / 1000);

	if (gProfiling)
	{
		QueryPerformanceFrequency(&gFrequency);
	}

	HANDLE hFrameTimer = CreateWaitableTimerExW(
		NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

	if (!hFrameTimer)
	{
		Debug() << "No high-resolution timer (err " << std::hex << GetLastError()
		     << "), using Sleep" << std::endl;
	}

	if (!Handover::Open())
	{
		Debug() << "No input event (err " << std::hex << GetLastError()
		     << "), input is a frame late" << std::endl;
	}

	if (bServe)
	{
		int ScreenWidth = 0;
		int ScreenHeight = 0;

		if (Desktop::ScreenSize(ScreenWidth, ScreenHeight))
		{
			if (!CreateServer(ScreenWidth, ScreenHeight, Port))
			{
				if (hFrameTimer)
				{
					CloseHandle(hFrameTimer);
				}

				Handover::Close();

				return false;
			}
		}
	}

	ULONGLONG NextFrame = GetTickCount64();
	ULONGLONG LastFrame = 0;
	ULONGLONG ActiveUntil = 0;

	const unsigned IdleFramesBeforeBackoff = 4;
	const unsigned MaxBackoff = 8;

	unsigned IdleFrames = 0;
	unsigned Backoff = 1;

	const ULONGLONG EmptyGraceMs = 2000;

	const ULONGLONG WindowCheckIntervalMs = 250;

	ULONGLONG EmptySince = 0;
	ULONGLONG NextWindowCheck = 0;

	FrameProfile::Start();

	bool bContinue = true;

	while (bContinue)
	{
		const ULONGLONG Now = GetTickCount64();

		if (Now >= NextWindowCheck)
		{
			NextWindowCheck = Now + WindowCheckIntervalMs;

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
				const HANDLE Program = Desktop::ProgramProcess();
				DWORD Code = 0;

				if (Program && GetExitCodeProcess(Program, &Code))
				{
					if (Code == STILL_ACTIVE)
					{
						Debug() << "Desktop empty, program still running"
						        << std::endl;
					}
					else
					{
						Debug() << "The program exited (code " << Code << "); exiting." << std::endl;
					}
				}
				else
				{
					Debug() << "Desktop empty" << std::endl;
				}

				bContinue = false;
			}
		}

		if (!bServe)
		{
			WaitForFrameOrInput(hFrameTimer, FrameIntervalMs);

			NextFrame = GetTickCount64();

			continue;
		}

		LARGE_INTEGER InputStart;
		BeginTimer(InputStart);

		if (gScreen && Handover::Apply(gScreen))
		{
			const ULONGLONG AppliedAt = GetTickCount64();
			FrameSchedule::OnInput(AppliedAt, LastFrame, FrameIntervalMs,
				NextFrame, IdleFrames, Backoff);
			ActiveUntil = AppliedAt + 500;
		}

		const ULONGLONG InputUs = MicrosecondsSince(InputStart);

		const unsigned Viewers = CountClients();

		Clipboard::Publish(gScreen, Viewers);

		if (gScreen && !Viewers)
		{
			FrameProfile::Frame Skipped = {};
			Skipped.Skipped = true;

			FrameProfile::Add(Skipped);
			FrameProfile::Flush(FrameIntervalUs);

			WaitForFrameOrInput(hFrameTimer, FrameIntervalMs);

			NextFrame = GetTickCount64();

			continue;
		}

		{
			const ULONGLONG Before = GetTickCount64();

			if (Before < NextFrame && WaitForFrameOrInput(hFrameTimer, (DWORD)(NextFrame - Before)))
			{
				if (GetTickCount64() < NextFrame)
				{
					continue;
				}
			}
		}

		LastFrame = GetTickCount64();

		Desktop::Frame Captured;

		LARGE_INTEGER CaptureStart;
		BeginTimer(CaptureStart);

		const bool bCaptured = Desktop::Capture(Captured);

		ULONGLONG CaptureUs = MicrosecondsSince(CaptureStart);

		LARGE_INTEGER DockStart;
		BeginTimer(DockStart);

		if (bCaptured)
		{
			int PointerX = 0;
			int PointerY = 0;

			Input::PointerPosition(PointerX, PointerY);

			Dock::Draw(Captured.Dc, Captured.Width, Captured.Height, PointerX, PointerY);
		}

		const ULONGLONG DockUs = MicrosecondsSince(DockStart);

		LARGE_INTEGER FinishStart;
		BeginTimer(FinishStart);

		if (bCaptured)
		{
			Desktop::Finish(Captured);
		}

		CaptureUs += MicrosecondsSince(FinishStart);

		if (bCaptured && !gScreen && !CreateServer(Captured.Width, Captured.Height, Port))
		{
			if (hFrameTimer)
			{
				CloseHandle(hFrameTimer);
			}

			Handover::Close();

			return false;
		}

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
		Sample.BytesSent = gProfiling ? BytesSent() : 0;

		FrameProfile::Add(Sample);
		FrameProfile::Flush(FrameIntervalUs);

		{
			const unsigned long long Still =
				(unsigned long long)Captured.Width * Captured.Height / 100;

			if (bCaptured && LastFrame >= ActiveUntil && ChangedArea <= Still)
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

		CarryUs += FrameIntervalUs * Backoff;

		const ULONGLONG AdvanceMs = CarryUs / 1000;

		CarryUs -= AdvanceMs * 1000;

		NextFrame += AdvanceMs;

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

	if (gScreen)
	{
#if defined(LIBVNCSERVER_HAVE_WIN32THREADS)
		HANDLE Listener = NULL;
		const bool HaveListener = gScreen->listener_thread
			&& gScreen->listener_thread != (uintptr_t)-1;
		const bool CanWait = HaveListener
			&& DuplicateHandle(GetCurrentProcess(), (HANDLE)gScreen->listener_thread,
				GetCurrentProcess(), &Listener, SYNCHRONIZE, FALSE, 0);
#endif

		rfbShutdownServer(gScreen, FALSE);

#if defined(LIBVNCSERVER_HAVE_WIN32THREADS)
		const bool ListenerStopped = CanWait && WaitForSingleObject(Listener, 2000) == WAIT_OBJECT_0;
		if (Listener) { CloseHandle(Listener); }
		if (!ListenerStopped)
		{
			Debug() << "VNC listener did not stop" << std::endl;
			gScreen = NULL;
			return true;
		}
#endif

#if defined(LIBVNCSERVER_HAVE_WIN32THREADS)
		if (!CloseClientConnections(gScreen))
		{
			Debug() << "A VNC client thread did not stop" << std::endl;
			gScreen = NULL;
			return true;
		}
#endif

		rfbScreenCleanup(gScreen);
		gScreen = NULL;
	}

	Handover::Close();

	return true;
}
