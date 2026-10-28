#include "Dock.h"

#include <Windows.h>

#include <algorithm>
#include <iostream>
#include <vector>

#include "Common/LauncherEnv.h"
#include "Common/LauncherLog.h"

using namespace std;

namespace
{
	// Read once: this is asked for on every frame.
	bool Enabled()
	{
		static const bool On = LauncherDock();

		return On;
	}

	int Height()
	{
		return Enabled() ? LAUNCHER_DOCK_HEIGHT : 0;
	}

	// One button per window, in the order the windows first appeared -- deliberately not
	// z-order, which changes on every click and would reshuffle buttons under the cursor.
	// A window is appended when it shows up and dropped when it goes away.
	std::vector<HWND> gButtons;

	const int ButtonWidth = 150;
	const int ButtonGap = 2;

	bool IsDockWindow(HWND hwnd)
	{
		if (!IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) <= 0)
		{
			return false;
		}

		// A taskbar does not list tool windows either. This also keeps the
		// desktop's own helper windows out of the strip.
		if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
		{
			return false;
		}

		return true;
	}

	BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lParam)
	{
		std::vector<HWND>* Found = (std::vector<HWND>*)lParam;

		if (IsDockWindow(hwnd))
		{
			Found->push_back(hwnd);
		}

		return TRUE;
	}

	// Drops windows that are gone, appends the ones that appeared, and leaves the rest
	// where they were.
	void Refresh()
	{
		std::vector<HWND> Found;

		EnumWindows(EnumProc, (LPARAM)&Found);

		std::vector<HWND> Ordered;

		for (size_t i = 0; i < gButtons.size(); ++i)
		{
			if (std::find(Found.begin(), Found.end(), gButtons[i]) != Found.end())
			{
				Ordered.push_back(gButtons[i]);
			}
		}

		for (size_t i = 0; i < Found.size(); ++i)
		{
			if (std::find(Ordered.begin(), Ordered.end(), Found[i]) == Ordered.end())
			{
				Ordered.push_back(Found[i]);
			}
		}

		gButtons.swap(Ordered);
	}

	// Brings a window back and gives it the foreground.
	void ActivateWindow(HWND Target)
	{
		// Only when it is actually minimized: SW_RESTORE on a maximized window
		// would undo the maximization instead of raising it.
		if (IsIconic(Target))
		{
			ShowWindow(Target, SW_RESTORE);
		}

		if (SetForegroundWindow(Target))
		{
			return;
		}

		// Windows only lets the process that owns the foreground hand it to
		// another, and inside the container that is the program being viewed,
		// never this one. Attaching this thread to the foreground thread's input
		// queue makes it count as part of that thread, which is the classic shell
		// workaround for the same restriction.
		const HWND Foreground = GetForegroundWindow();
		const DWORD ForegroundThread = Foreground ? GetWindowThreadProcessId(Foreground, NULL) : 0;
		const DWORD ThisThread = GetCurrentThreadId();

		if (ForegroundThread && ForegroundThread != ThisThread)
		{
			AttachThreadInput(ThisThread, ForegroundThread, TRUE);
			SetForegroundWindow(Target);
			AttachThreadInput(ThisThread, ForegroundThread, FALSE);
		}
	}

	// Where button Index starts, and how far apart the buttons are.
	int ButtonLeft(int Index)
	{
		return ButtonGap + Index * (ButtonWidth + ButtonGap);
	}

	// Narrows the desktop's work area to leave the bottom Rows to the dock, the way a
	// taskbar reserves the space it occupies, so maximized windows stop above it. The
	// screen is unaffected: this changes what programs are told they can use.
	void ReserveScreenBottom(int Rows)
	{
		RECT Area = { 0 };
		Area.right = GetSystemMetrics(SM_CXSCREEN);
		Area.bottom = GetSystemMetrics(SM_CYSCREEN) - Rows;

		// No SPIF_SENDCHANGE: this runs before the program is started, so there
		// is nothing yet that has cached the old work area, and the setting
		// persists for whatever starts later.
		if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &Area, 0))
		{
			Debug() << "Work area could not be reserved (err 0x" << hex << GetLastError() << ")" << endl;

			return;
		}

		// Read it back rather than trusting the return value. This is the one part
		// of the dock the container cannot show us, and a work area that quietly
		// stayed full-screen would leave every maximized window behind it.
		RECT Got = { 0 };

		if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &Got, 0)
			&& (Got.right != Area.right || Got.bottom != Area.bottom))
		{
			Debug() << "Work area stayed " << Got.right << "x" << Got.bottom
			        << " rather than " << Area.right << "x" << Area.bottom << endl;

			return;
		}

		Debug() << "Work area reserved: bottom " << Rows << " rows" << endl;
	}
}

void Dock::Open()
{
	if (!Enabled())
	{
		return;
	}

	ReserveScreenBottom(LAUNCHER_DOCK_HEIGHT);
}

bool Dock::Contains(int Y, int FrameHeight)
{
	return Enabled() && Y >= FrameHeight - LAUNCHER_DOCK_HEIGHT;
}

void Dock::Draw(HDC Dc, int Width, int Height)
{
	if (!Enabled())
	{
		return;
	}

	Refresh();

	// Created once. Three GDI objects per frame, thirty times a second, is churn
	// for no reason.
	static HBRUSH Background = CreateSolidBrush(RGB(32, 32, 32));
	static HBRUSH Edge = CreateSolidBrush(RGB(70, 70, 70));
	static HBRUSH Button = CreateSolidBrush(RGB(58, 58, 58));
	static HBRUSH Active = CreateSolidBrush(RGB(0, 90, 158));
	static HBRUSH Minimized = CreateSolidBrush(RGB(44, 44, 44));
	static HFONT Font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

	POINT OldOrigin;
	SetViewportOrgEx(Dc, 0, Height - LAUNCHER_DOCK_HEIGHT, &OldOrigin);

	RECT Strip{ 0, 0, Width, LAUNCHER_DOCK_HEIGHT };
	FillRect(Dc, &Strip, Background);

	// A line along the top, so the dock reads as chrome rather than as part of
	// whatever window is behind it.
	RECT Line{ 0, 0, Width, 1 };
	FillRect(Dc, &Line, Edge);

	HGDIOBJ OldFont = SelectObject(Dc, Font);
	SetBkMode(Dc, TRANSPARENT);

	const HWND Foreground = GetForegroundWindow();

	for (size_t i = 0; i < gButtons.size(); ++i)
	{
		const int x = ButtonLeft((int)i);

		if (x + ButtonWidth > Width)
		{
			break;
		}

		const HWND hwnd = gButtons[i];
		const bool bMinimized = IsIconic(hwnd) != FALSE;

		// A minimized window can still be the foreground window: minimizing it does not
		// necessarily hand the foreground anywhere, so testing the foreground alone would
		// light up the button of a window that is not on screen at all.
		const bool bActive = !bMinimized && hwnd == Foreground;

		RECT Cell{ x, 2, x + ButtonWidth, LAUNCHER_DOCK_HEIGHT - 2 };
		FillRect(Dc, &Cell, bActive ? Active : (bMinimized ? Minimized : Button));

		WCHAR Title[128] = { 0 };
		GetWindowTextW(hwnd, Title, 128);

		SetTextColor(Dc, bActive ? RGB(255, 255, 255)
			: (bMinimized ? RGB(150, 150, 150) : RGB(225, 225, 225)));

		RECT Text = Cell;
		Text.left += 6;
		Text.right -= 6;

		// DT_NOPREFIX: an & in a window title is a literal character here, not an
		// accelerator to underline.
		DrawTextW(Dc, Title, -1, &Text,
			DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
	}

	SelectObject(Dc, OldFont);
	SetViewportOrgEx(Dc, OldOrigin.x, OldOrigin.y, NULL);
}

void Dock::Press(int X, int Y, int Width, int FrameHeight)
{
	if (!Contains(Y, FrameHeight))
	{
		return;
	}

	Refresh();

	const int Pitch = ButtonWidth + ButtonGap;

	if (X < ButtonGap || X >= Width)
	{
		return;
	}

	// A click in the gap between two buttons belongs to neither of them.
	if ((X - ButtonGap) % Pitch >= ButtonWidth)
	{
		return;
	}

	const int Index = (X - ButtonGap) / Pitch;

	// Buttons that did not fit were never drawn, so there is nothing there to
	// have clicked on.
	if (Index < 0 || (size_t)Index >= gButtons.size())
	{
		return;
	}

	const HWND Target = gButtons[Index];

	if (!IsWindow(Target))
	{
		return;
	}

	if (IsIconic(Target))
	{
		ActivateWindow(Target);
	}
	else if (Target == GetForegroundWindow())
	{
		// The toggle a taskbar button has. It is also the only way to minimize a
		// window whose title bar is off-screen.
		ShowWindow(Target, SW_MINIMIZE);
	}
	else
	{
		ActivateWindow(Target);
	}
}
