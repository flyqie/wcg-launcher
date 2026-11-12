#include "Dock.h"

#include <Windows.h>
#include <Shlwapi.h>

#include <algorithm>
#include <cstddef>
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

	bool ReadProcessPath(HWND Window, wchar_t* Path, DWORD Length)
	{
		DWORD ProcessId = 0;

		GetWindowThreadProcessId(Window, &ProcessId);

		if (!ProcessId)
		{
			return false;
		}

		HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ProcessId);

		if (!Process)
		{
			return false;
		}

		const BOOL Read = QueryFullProcessImageNameW(Process, 0, Path, &Length);

		CloseHandle(Process);

		return Read != FALSE;
	}

	struct DockButton
	{
		HWND Window = NULL;
		std::wstring Title;

		HICON Icon = NULL;
		unsigned IconTries = 0;
	};

	HICON AcquireIcon(HWND Window)
	{
		ULONG_PTR Result = 0;

		static const WPARAM Kinds[] = { ICON_SMALL2, ICON_SMALL };

		for (const WPARAM Kind : Kinds)
		{
			Result = 0;

			SendMessageTimeoutW(Window, WM_GETICON, Kind, 0,
				SMTO_ABORTIFHUNG, 25, &Result);

			if (Result)
			{
				break;
			}
		}

		if (!Result)
		{
			Result = GetClassLongPtrW(Window, GCLP_HICON);
		}

		if (!Result)
		{
			Result = GetClassLongPtrW(Window, GCLP_HICONSM);
		}

		if (Result)
		{
			HICON Copy = CopyIcon((HICON)Result);

			if (Copy)
			{
				return Copy;
			}
		}

		wchar_t Path[MAX_PATH] = { 0 };

		if (ReadProcessPath(Window, Path, MAX_PATH))
		{
			HICON Extracted = ExtractIconW(NULL, Path, 0);

			if (Extracted && Extracted != (HICON)1)
			{
				return Extracted;
			}
		}

		return NULL;
	}

	void ReleaseIcon(DockButton& Entry)
	{
		if (Entry.Icon)
		{
			DestroyIcon(Entry.Icon);

			Entry.Icon = NULL;
		}
	}

	std::vector<DockButton> gButtons;

	ULONGLONG gNextPoll = 0;

	const int ButtonWidthMax = 150;
	const int ButtonWidthMin = 64;
	const int ButtonGap = 2;

	const ULONGLONG PollIntervalMs = 250;

	const wchar_t* const ShellProcessMarks[] =
	{
		L"\\explorer.exe",
		L"\\Windows\\SystemApps\\",
		L"\\WindowsApps\\",
		L"\\MicrosoftEdge\\",
		L"\\SystemSettings.exe",
		L"\\ApplicationFrameHost.exe",
	};

	bool ScreenExtent(int& Width, int& Height)
	{
		DEVMODEW Mode = { 0 };

		Mode.dmSize = sizeof(Mode);

		if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &Mode))
		{
			Width = GetSystemMetrics(SM_CXSCREEN);
			Height = GetSystemMetrics(SM_CYSCREEN);

			return Width > 0 && Height > 0;
		}

		Width = (int)Mode.dmPelsWidth;
		Height = (int)Mode.dmPelsHeight;

		return true;
	}

	bool IsShellProcess(HWND Window)
	{
		wchar_t Path[MAX_PATH] = { 0 };

		if (!ReadProcessPath(Window, Path, MAX_PATH))
		{
			return false;
		}

		for (const wchar_t* Mark : ShellProcessMarks)
		{
			if (StrStrIW(Path, Mark))
			{
				return true;
			}
		}

		return false;
	}

	bool IsDockWindow(HWND Window)
	{
		if (!IsWindow(Window) || !IsWindowVisible(Window))
		{
			return false;
		}

		const LONG_PTR Style = GetWindowLongPtrW(Window, GWL_EXSTYLE);

		if (Style & WS_EX_TOOLWINDOW)
		{
			return false;
		}

		if (GetWindow(Window, GW_OWNER) && !(Style & WS_EX_APPWINDOW))
		{
			return false;
		}

		return !IsShellProcess(Window);
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

	std::wstring ReadTitle(HWND Window)
	{
		wchar_t Title[128] = { 0 };

		if (!InternalGetWindowText(Window, Title, (int)(sizeof(Title) / sizeof(Title[0]))))
		{
			GetWindowTextW(Window, Title, (int)(sizeof(Title) / sizeof(Title[0])));
		}

		return Title;
	}

	const unsigned IconTriesMax = 8;

	void RefreshLabels(std::vector<DockButton>& List)
	{
		bool bIconAsked = false;

		for (DockButton& Entry : List)
		{
			Entry.Title = ReadTitle(Entry.Window);

			if (!bIconAsked && !Entry.Icon && Entry.IconTries < IconTriesMax)
			{
				bIconAsked = true;

				++Entry.IconTries;

				Entry.Icon = AcquireIcon(Entry.Window);
			}
		}
	}

	void Reconcile()
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

				Existing.Icon = NULL;
			}
			else
			{
				ReleaseIcon(Existing);
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

		RefreshLabels(Merged);

		for (DockButton& Entry : Merged)
		{
			if (Entry.Title.empty())
			{
				wchar_t Class[64] = { 0 };

				GetClassNameW(Entry.Window, Class, (int)(sizeof(Class) / sizeof(Class[0])));

				Entry.Title = Class;
			}

			if (!Entry.Title.empty())
			{
				Listed.push_back(std::move(Entry));

				Entry.Icon = NULL;
			}
			else
			{
				ReleaseIcon(Entry);
			}
		}

		gButtons.swap(Listed);
	}

	void DropClosed()
	{
		for (size_t i = gButtons.size(); i-- > 0;)
		{
			if (!IsWindow(gButtons[i].Window) || !IsWindowVisible(gButtons[i].Window))
			{
				ReleaseIcon(gButtons[i]);

				gButtons.erase(gButtons.begin() + (ptrdiff_t)i);
			}
		}
	}

	void Refresh()
	{
		const ULONGLONG Now = GetTickCount64();

		if (Now >= gNextPoll)
		{
			gNextPoll = Now + PollIntervalMs;

			Reconcile();

			return;
		}

		DropClosed();
	}

	void ActivateWindow(HWND Target)
	{
		SwitchToThisWindow(Target, TRUE);
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
		int ScreenWidth = 0;
		int ScreenHeight = 0;

		if (!ScreenExtent(ScreenWidth, ScreenHeight))
		{
			return;
		}

		RECT Area = { 0 };
		Area.right = ScreenWidth;
		Area.bottom = ScreenHeight - Rows;

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
	const int SmallIcon = GetSystemMetrics(SM_CXSMICON);

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

		if (Entry.Icon)
		{
			const int Room = Cell.bottom - Cell.top - 2;
			const int IconSize = (SmallIcon < Room) ? SmallIcon : Room;
			const int IconTop = Cell.top + (Cell.bottom - Cell.top - IconSize) / 2;

			DrawIconEx(Dc, Text.left, IconTop, Entry.Icon, IconSize, IconSize, 0, NULL, DI_NORMAL);

			Text.left += IconSize + 4;
		}

		if (Text.right > Text.left)
		{
			DrawTextW(Dc, Entry.Title.c_str(), -1, &Text,
				DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
		}
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

	const int Index = ButtonAt(X, Width);

	if (Index < 0)
	{
		return;
	}

	const HWND Target = gButtons[Index].Window;

	if (!IsWindow(Target))
	{
		Debug() << "Dock button for a window that no longer exists" << std::endl;

		DropClosed();

		return;
	}

	if (IsIconic(Target) || Target != GetForegroundWindow())
	{
		ActivateWindow(Target);
	}
	else
	{
		ShowWindowAsync(Target, SW_MINIMIZE);
	}
}

void Dock::Close(int X, int Y, int Width, int FrameHeight)
{
	if (!Contains(Y, FrameHeight))
	{
		return;
	}

	const int Index = ButtonAt(X, Width);

	if (Index < 0)
	{
		return;
	}

	const HWND Target = gButtons[Index].Window;

	if (!IsWindow(Target))
	{
		Debug() << "Dock button for a window that no longer exists" << std::endl;

		DropClosed();

		return;
	}

	PostMessageW(Target, WM_CLOSE, 0, 0);
}
