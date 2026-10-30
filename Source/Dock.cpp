#include "Dock.h"

#include <Windows.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "Common/LauncherEnv.h"
#include "Common/LauncherLog.h"

namespace
{
	bool Enabled()
	{
		static const bool On = LauncherDock();

		return On;
	}

	int StripHeight()
	{
		static const int Value = []
		{
			if (!Enabled())
			{
				return 0;
			}

			HDC Screen = GetDC(NULL);

			if (!Screen)
			{
				return (int)LAUNCHER_DOCK_HEIGHT;
			}

			const int Dpi = GetDeviceCaps(Screen, LOGPIXELSY);

			ReleaseDC(NULL, Screen);

			if (Dpi <= 0)
			{
				return (int)LAUNCHER_DOCK_HEIGHT;
			}

			return (int)((LAUNCHER_DOCK_HEIGHT * Dpi + 48) / 96);
		}();

		return Value;
	}

	struct DockButton
	{
		HWND Window = NULL;
		std::wstring Title;
		unsigned long long ReadAt = 0;
	};

	std::vector<DockButton> gButtons;

	unsigned long long gFrameCounter = 0;

	const unsigned long long TitleRefreshFrames = 15;

	const int ButtonWidthMax = 150;
	const int ButtonWidthMin = 44;
	const int ButtonGap = 2;

	bool IsDockWindow(HWND hwnd)
	{
		if (!IsWindowVisible(hwnd))
		{
			return false;
		}

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

	void Refresh()
	{
		static std::vector<HWND> Found;
		static std::vector<DockButton> Merged;
		static std::vector<DockButton> Listed;
		static std::unordered_set<HWND> Present;
		static std::unordered_set<HWND> Kept;

		Found.clear();
		Merged.clear();
		Listed.clear();
		Present.clear();
		Kept.clear();

		EnumWindows(EnumProc, (LPARAM)&Found);

		Present.insert(Found.begin(), Found.end());

		for (DockButton& Existing : gButtons)
		{
			if (Present.count(Existing.Window))
			{
				Kept.insert(Existing.Window);

				Merged.push_back(std::move(Existing));
			}
		}

		for (HWND hwnd : Found)
		{
			if (!Kept.count(hwnd))
			{
				DockButton New;

				New.Window = hwnd;

				Merged.push_back(std::move(New));
			}
		}

		for (DockButton& Entry : Merged)
		{
			if (!Entry.ReadAt || gFrameCounter - Entry.ReadAt >= TitleRefreshFrames)
			{
				WCHAR Title[128] = { 0 };

				GetWindowTextW(Entry.Window, Title, 128);

				Entry.Title = Title;
				Entry.ReadAt = gFrameCounter;
			}

			if (!Entry.Title.empty())
			{
				Listed.push_back(std::move(Entry));
			}
		}

		gButtons.swap(Listed);
	}

	void ActivateWindow(HWND Target)
	{
		if (IsIconic(Target))
		{
			ShowWindow(Target, SW_RESTORE);
		}

		if (SetForegroundWindow(Target))
		{
			return;
		}

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

	int ButtonWidthFor(int Width, size_t Count)
	{
		if (!Count || Width <= 0)
		{
			return ButtonWidthMax;
		}

		const int Room = Width - (int)(Count + 1) * ButtonGap;

		if (Room <= 0)
		{
			return ButtonWidthMin;
		}

		const int Fit = Room / (int)Count;

		return (Fit < ButtonWidthMin) ? ButtonWidthMin
			: (Fit > ButtonWidthMax) ? ButtonWidthMax : Fit;
	}

	int ButtonLeft(int Index, int Width)
	{
		return ButtonGap + Index * (ButtonWidthFor(Width, gButtons.size()) + ButtonGap);
	}

	int ButtonAt(int X, int Width)
	{
		const int Pitch = ButtonWidthFor(Width, gButtons.size()) + ButtonGap;

		if (X < ButtonGap || X >= Width)
		{
			return -1;
		}

		if ((X - ButtonGap) % Pitch >= ButtonWidthFor(Width, gButtons.size()))
		{
			return -1;
		}

		const int Index = (X - ButtonGap) / Pitch;

		if (Index < 0 || (size_t)Index >= gButtons.size())
		{
			return -1;
		}

		return Index;
	}

	void ReserveScreenBottom(int Rows)
	{
		RECT Area = { 0 };
		Area.right = GetSystemMetrics(SM_CXSCREEN);
		Area.bottom = GetSystemMetrics(SM_CYSCREEN) - Rows;

		if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &Area, 0))
		{
			Debug() << "Work area could not be reserved (err 0x" << std::hex << GetLastError() << ")" << std::endl;

			return;
		}

		RECT Got = { 0 };

		if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &Got, 0)
			&& (Got.right != Area.right || Got.bottom != Area.bottom))
		{
			Debug() << "Work area stayed " << Got.right << "x" << Got.bottom
			        << " rather than " << Area.right << "x" << Area.bottom << std::endl;

			return;
		}

		Debug() << "Work area reserved: bottom " << Rows << " rows" << std::endl;
	}

	void DrawButtonEdge(HDC Dc, const RECT& Cell, bool bSunken, HBRUSH Highlight, HBRUSH Shadow)
	{
		const HBRUSH First = bSunken ? Shadow : Highlight;
		const HBRUSH Second = bSunken ? Highlight : Shadow;

		RECT Top{ Cell.left, Cell.top, Cell.right, Cell.top + 1 };
		RECT Left{ Cell.left, Cell.top, Cell.left + 1, Cell.bottom };
		RECT Bottom{ Cell.left, Cell.bottom - 1, Cell.right, Cell.bottom };
		RECT Right{ Cell.right - 1, Cell.top, Cell.right, Cell.bottom };

		FillRect(Dc, &Top, First);
		FillRect(Dc, &Left, First);
		FillRect(Dc, &Bottom, Second);
		FillRect(Dc, &Right, Second);
	}
}

void Dock::Open()
{
	if (!Enabled())
	{
		return;
	}

	ReserveScreenBottom(StripHeight());
}

bool Dock::Contains(int Y, int FrameHeight)
{
	return Enabled() && Y >= FrameHeight - StripHeight();
}

void Dock::Draw(HDC Dc, int Width, int Height, int PointerX, int PointerY)
{
	if (!Enabled())
	{
		return;
	}

	++gFrameCounter;

	Refresh();

	static HBRUSH Background = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
	static HBRUSH Hover = CreateSolidBrush(GetSysColor(COLOR_3DLIGHT));
	static HBRUSH Active = CreateSolidBrush(GetSysColor(COLOR_ACTIVECAPTION));
	static HBRUSH Highlight = CreateSolidBrush(GetSysColor(COLOR_3DHIGHLIGHT));
	static HBRUSH Shadow = CreateSolidBrush(GetSysColor(COLOR_3DSHADOW));

	static const COLORREF TextNormal = GetSysColor(COLOR_BTNTEXT);
	static const COLORREF TextActive = GetSysColor(COLOR_CAPTIONTEXT);
	static const COLORREF TextMinimized = GetSysColor(COLOR_GRAYTEXT);

	static HFONT Font = []
	{
		NONCLIENTMETRICSW Metrics = { 0 };
		Metrics.cbSize = sizeof(Metrics);

		if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(Metrics), &Metrics, 0))
		{
			HFONT Made = CreateFontIndirectW(&Metrics.lfMenuFont);

			if (Made)
			{
				return Made;
			}
		}

		return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	}();

	const int Strip = StripHeight();

	POINT OldOrigin;
	SetViewportOrgEx(Dc, 0, Height - Strip, &OldOrigin);

	RECT Bar{ 0, 0, Width, Strip };
	FillRect(Dc, &Bar, Background);

	RECT LineHighlight{ 0, 0, Width, 1 };
	RECT LineShadow{ 0, 1, Width, 2 };
	FillRect(Dc, &LineHighlight, Highlight);
	FillRect(Dc, &LineShadow, Shadow);

	HGDIOBJ OldFont = SelectObject(Dc, Font);
	SetBkMode(Dc, TRANSPARENT);

	const HWND Foreground = GetForegroundWindow();
	const int PointerIndex = (PointerY >= Height - Strip) ? ButtonAt(PointerX, Width) : -1;

	for (size_t i = 0; i < gButtons.size(); ++i)
	{
		const int ButtonWidth = ButtonWidthFor(Width, gButtons.size());
		const int x = ButtonLeft((int)i, Width);

		if (x + ButtonWidth > Width)
		{
			break;
		}

		const DockButton& Entry = gButtons[i];
		const HWND hwnd = Entry.Window;
		const bool bMinimized = IsIconic(hwnd) != FALSE;

		const bool bActive = !bMinimized && hwnd == Foreground;

		RECT Cell{ x, 2, x + ButtonWidth, Strip - 2 };

		HBRUSH Fill = bActive ? Active : ((int)i == PointerIndex ? Hover : Background);

		FillRect(Dc, &Cell, Fill);

		DrawButtonEdge(Dc, Cell, bActive, Highlight, Shadow);

		SetTextColor(Dc, bActive ? TextActive : (bMinimized ? TextMinimized : TextNormal));

		RECT Text = Cell;
		Text.left += 6;
		Text.right -= 6;

		DrawTextW(Dc, Entry.Title.c_str(), -1, &Text,
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

	const int Index = ButtonAt(X, Width);

	if (Index < 0)
	{
		return;
	}

	const HWND Target = gButtons[Index].Window;

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
		ShowWindow(Target, SW_MINIMIZE);
	}
	else
	{
		ActivateWindow(Target);
	}
}

void Dock::Close(int X, int Y, int Width, int FrameHeight)
{
	if (!Contains(Y, FrameHeight))
	{
		return;
	}

	Refresh();

	const int Index = ButtonAt(X, Width);

	if (Index < 0)
	{
		return;
	}

	const HWND Target = gButtons[Index].Window;

	if (!IsWindow(Target))
	{
		return;
	}

	PostMessageW(Target, WM_CLOSE, 0, 0);
}
