// OCR_NORMAL, for the cursor fallback below. It has to be defined before
// Windows.h is first included, and Desktop.h includes it.
#define OEMRESOURCE

#include "Desktop.h"

#include <Windows.h>
#include <AclAPI.h>
#include <shellapi.h>

#include <iostream>
#include <string.h>
#include <string>
#include <vector>

#include "Common/LauncherEnv.h"
#include "Common/LauncherLog.h"

using namespace std;

namespace
{
	// --- the window station -------------------------------------------------
	//
	// A container's processes start on the service window station (Service-0x0-xxx$),
	// where the rights are too low to create windows or switch desktop, and where
	// PrintWindow produces black. Moving to WinSta0\Default needs both opened to
	// everyone first.

	bool AdjustSingleTokenPrivilege(HANDLE TokenHandle, LPCTSTR lpName, DWORD dwAttributes)
	{
		TOKEN_PRIVILEGES tp;
		tp.PrivilegeCount = 1;
		tp.Privileges[0].Attributes = dwAttributes;

		if (!LookupPrivilegeValue(NULL, lpName, &(tp.Privileges[0].Luid)))
		{
			return false;
		}

		if (!AdjustTokenPrivileges(TokenHandle, FALSE, &tp, 0, NULL, NULL))
		{
			return false;
		}

		return true;
	}

	void TweakWindowStation()
	{
		HWINSTA hWinSta = OpenWindowStation(TEXT("WinSta0"), FALSE, GENERIC_EXECUTE | WRITE_DAC);

		if (!hWinSta)
		{
			Debug() << "Opening WinSta0 failed (err 0x" << hex << GetLastError() << ")" << endl;
			return;
		}

		HWINSTA hOldWinsta = GetProcessWindowStation();
		SetProcessWindowStation(hWinSta);

		HDESK hDesktop = OpenDesktop(TEXT("Default"), 0, FALSE, GENERIC_READ | WRITE_DAC);

		if (!hDesktop)
		{
			Debug() << "Opening Default desktop failed (err 0x" << hex << GetLastError() << ")" << endl;
			return;
		}

		SID_IDENTIFIER_AUTHORITY SIDAuthWorld = SECURITY_WORLD_SID_AUTHORITY;

		PSID pEveryoneSID = NULL;
		AllocateAndInitializeSid(&SIDAuthWorld, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &pEveryoneSID);

		EXPLICIT_ACCESS ExplicitAccess;
		memset(&ExplicitAccess, 0, sizeof(ExplicitAccess));
		ExplicitAccess.grfAccessPermissions = GENERIC_ALL;
		ExplicitAccess.grfAccessMode = GRANT_ACCESS;
		ExplicitAccess.grfInheritance = NO_INHERITANCE;
		ExplicitAccess.Trustee.pMultipleTrustee = NULL;
		ExplicitAccess.Trustee.MultipleTrusteeOperation = NO_MULTIPLE_TRUSTEE;
		ExplicitAccess.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		ExplicitAccess.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
		ExplicitAccess.Trustee.ptstrName = (TCHAR*)pEveryoneSID;

		PACL Dacl;
		DWORD Status = SetEntriesInAcl(1, &ExplicitAccess, NULL, &Dacl);

		if (Status == ERROR_SUCCESS)
		{
			HANDLE hToken;
			OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken);

			if (AdjustSingleTokenPrivilege(hToken, SE_SECURITY_NAME, SE_PRIVILEGE_ENABLED))
			{
				Status = SetSecurityInfo(hWinSta, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
					NULL, NULL, Dacl, NULL);

				if (Status)
				{
					Debug() << "SetSecurityInfo returned " << hex << Status << endl;
				}

				Status = SetSecurityInfo(hDesktop, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
					NULL, NULL, Dacl, NULL);

				if (Status)
				{
					Debug() << "SetSecurityInfo returned " << hex << Status << endl;
				}
			}
			else
			{
				Debug() << "AdjustTokenPrivileges failed (err 0x" << hex << GetLastError() << ")" << endl;
			}
		}
		else
		{
			Debug() << "SetEntriesInAcl returned " << hex << Status << endl;
		}

		LocalFree(Dacl);

		SetProcessWindowStation(hOldWinsta);
		CloseHandle(hWinSta);
		CloseHandle(hDesktop);
	}

	// Moves the process onto WinSta0\Default. False if the desktop could not be reached,
	// in which case there is nothing to show and nothing to send input to.
	bool AssignWindowStation()
	{
		HWINSTA hWinSta = CreateWindowStation(TEXT("WinSta0"), 0, GENERIC_ALL, NULL);

		if (!hWinSta)
		{
			Debug() << "Opening WinSta0 failed (err 0x" << hex << GetLastError() << ")" << endl;
			return false;
		}

		HWINSTA hOldWinsta = GetProcessWindowStation();

		if (!SetProcessWindowStation(hWinSta))
		{
			Debug() << "SetProcessWindowStation failed (err 0x" << hex << GetLastError() << ")" << endl;
			return false;
		}

		HDESK hDesktop = CreateDesktop(TEXT("Default"), NULL, NULL, 0, GENERIC_ALL, NULL);

		if (!hDesktop)
		{
			Debug() << "Opening hDesktop failed (err 0x" << hex << GetLastError() << ")" << endl;
			return false;
		}

		HDESK hOldDesktop = GetThreadDesktop(GetCurrentThreadId());

		if (!SetThreadDesktop(hDesktop))
		{
			Debug() << "SetThreadDesktop failed (err 0x" << hex << GetLastError() << ")" << endl;
			return false;
		}

		if (hOldWinsta)
		{
			CloseHandle(hOldWinsta);
		}

		if (hOldDesktop)
		{
			CloseHandle(hOldDesktop);
		}

		USEROBJECTFLAGS Flags{ 0 };
		Flags.dwFlags = WSF_VISIBLE;

		if (!SetUserObjectInformation(hWinSta, UOI_FLAGS, &Flags, sizeof(Flags)))
		{
			Debug() << "SetUserObjectInformation failed (err 0x" << hex << GetLastError() << ")" << endl;
		}

		if (!SwitchDesktop(hDesktop))
		{
			Debug() << "SwitchDesktop failed (err 0x" << hex << GetLastError() << ")" << endl;
		}

		return true;
	}

	// The container has no shell to paint a background, so the client picks one.
	void SetDesktopColour()
	{
		INT Elements[] = { COLOR_DESKTOP };
		COLORREF RgbValues[] = { RGB(8, 110, 165) };

		SetSysColors(1, Elements, RgbValues);
	}

	// --- the display mode ---------------------------------------------------

	bool SetDisplayMode(int Width, int Height)
	{
		DEVMODEW Mode;
		memset(&Mode, 0, sizeof(Mode));

		Mode.dmSize = sizeof(Mode);

		// All four fields are required, not just the size.
		//
		// The stub display driver validates a requested mode by looking for it in
		// its mode table with the bit depth and refresh rate attached, and falls
		// back to the default when any of them is left unset. See
		// Docs/IMAGE-ANALYSIS.md.
		Mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
		Mode.dmPelsWidth = (DWORD)Width;
		Mode.dmPelsHeight = (DWORD)Height;
		Mode.dmBitsPerPel = 32;
		Mode.dmDisplayFrequency = 60;

		const LONG Result = ChangeDisplaySettingsW(&Mode, 0);

		if (Result != DISP_CHANGE_SUCCESSFUL)
		{
			Debug() << "ChangeDisplaySettings to " << Width << "x" << Height
			        << " rejected (code " << Result << ")" << endl;

			return false;
		}

		Debug() << "Display mode set to " << Width << "x" << Height << endl;

		return true;
	}

	// --- input --------------------------------------------------------------

	void Send(INPUT& Input, const char* What)
	{
		if (!SendInput(1, &Input, sizeof(INPUT)))
		{
			Debug() << "SendInput for " << What << " returned false with error "
			        << hex << GetLastError() << endl;
		}
	}

	// A window the desktop should be counted by.
	BOOL CALLBACK CountProc(HWND hwnd, LPARAM lParam)
	{
		DWORD* Count = (DWORD*)lParam;

		if (IsWindowVisible(hwnd))
		{
			*Count = *Count + 1;
		}

		return TRUE;
	}
}

bool Desktop::Open()
{
	Debug() << "Configuring window stations..." << endl;
	TweakWindowStation();

	Debug() << "Assigning current process to the default window station..." << endl;

	if (!AssignWindowStation())
	{
		return false;
	}

	SetDesktopColour();

	// Before the program starts, so it opens at the right size. The frame path adapts on
	// its own: the capture reads the screen size every frame and rebuilds when it changes,
	// and a new framebuffer goes to libvncserver at the new size.
	int ModeWidth = 0;
	int ModeHeight = 0;

	if (LauncherResolution(ModeWidth, ModeHeight))
	{
		SetDisplayMode(ModeWidth, ModeHeight);
	}

	return true;
}

void Desktop::StartProgram(const wchar_t* Command)
{
	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	memset(&si, 0, sizeof(si));
	memset(&pi, 0, sizeof(pi));

	si.cb = sizeof(si);

	HANDLE hDesktop = GetThreadDesktop(GetCurrentThreadId());
	HANDLE hWinSta = GetProcessWindowStation();

	DWORD TotalStringSize = 0;

	DWORD nWinStaNameSize = 0;
	GetUserObjectInformation(hWinSta, UOI_NAME, NULL, 0, &nWinStaNameSize);
	TotalStringSize += nWinStaNameSize;

	DWORD nDesktopNameSize = 0;
	GetUserObjectInformation(hDesktop, UOI_NAME, NULL, 0, &nDesktopNameSize);
	TotalStringSize += nDesktopNameSize;

	TCHAR* lpDesktop = new TCHAR[TotalStringSize / sizeof(TCHAR)];

	const DWORD nWinStaNameLength = nWinStaNameSize / sizeof(TCHAR);
	GetUserObjectInformation(hWinSta, UOI_NAME, lpDesktop, nWinStaNameSize, &nWinStaNameSize);
	lpDesktop[nWinStaNameLength - 1] = '\\';

	GetUserObjectInformation(hDesktop, UOI_NAME, lpDesktop + nWinStaNameLength, nDesktopNameSize, &nDesktopNameSize);

	DebugW() << L"Desktop: " << lpDesktop << endl;

	si.lpDesktop = lpDesktop;

	// cmd.exe unless the caller named something else.
	const wchar_t* ToRun = (Command && *Command) ? Command : L"cmd.exe";

	// CreateProcess may modify the command line, so it needs a writable copy.
	std::vector<wchar_t> CommandLine(ToRun, ToRun + wcslen(ToRun) + 1);

	BOOL Result = CreateProcess(
		NULL,
		CommandLine.data(),
		NULL,
		NULL,
		FALSE,
		CREATE_NEW_CONSOLE,
		NULL,
		NULL,
		&si,
		&pi
	);

	delete[] lpDesktop;

	if (!Result)
	{
		Debug() << "Process creation failed: error " << GetLastError() << endl;
		return;
	}

	Debug() << "Waiting for input..." << endl;
	WaitForInputIdle(pi.hProcess, INFINITE);

	// Wait until a window is drawn.
	//
	// A top-level window is not enough: a window is created before it is shown,
	// and GetTopWindow is satisfied by one that is still hidden. The session
	// below counts visible windows, so it would see an empty desktop and end on
	// its first frame, before the program had drawn anything at all.
	DWORD dwCount = 0;
	while (!HasVisibleWindows() && dwCount < 10000)
	{
		dwCount += 100;
		Sleep(100);
	}

	Debug() << "Done" << endl;
}

bool Desktop::HasVisibleWindows()
{
	DWORD Count = 0;

	EnumWindows(CountProc, (LPARAM)&Count);

	return Count != 0;
}

void Desktop::ReadCursor(Cursor& Out)
{
	Out = Cursor();

	CURSORINFO cursor = { sizeof(cursor) };
	if (!GetCursorInfo(&cursor))
	{
		return;
	}

	Out.X = cursor.ptScreenPos.x;
	Out.Y = cursor.ptScreenPos.y;

	// CURSOR_SUPPRESSED is the only state that means "do not draw this".
	//
	// Testing for CURSOR_SHOWING instead -- the obvious reading of the flag --
	// reports the cursor as hidden on this desktop. The container's window
	// station is created by the client rather than by winlogon, and it reports
	// flags 0 while still having a cursor worth drawing.
	Out.Visible = cursor.flags != CURSOR_SUPPRESSED;

	// What the desktop has, or the system arrow if it has nothing to give.
	//
	// GetCursorInfo reports the cursor the *session* is displaying, and a container has
	// no session: win32k only maintains that record when the calling thread's queue is
	// the session's active input queue. So it is NULL on most frames -- measured, 57 of
	// 57. The cursor is not missing; that record is, and the position is unaffected.
	//
	// GetCursor was tried and is not the way around it: it reads the calling thread's
	// *own* queue, which holds whatever this program last set, and this program never
	// calls SetCursor. It returned the same 32x32 wait cursor on every frame.
	//
	// So: use what the desktop gives, and the arrow when it gives nothing.
	HCURSOR hCursor = cursor.hCursor;

	Out.HandleMissing = hCursor == NULL;

	if (!hCursor)
	{
		// Loaded once rather than per frame: this runs on every frame the desktop reports
		// nothing, and LR_SHARED hands back the same handle every time. Because it is
		// stable, the cache below settles after the first of these and the bitmap is never
		// read twice -- a frame with no cursor costs one comparison.
		static HCURSOR Fallback = NULL;

		if (!Fallback)
		{
			Fallback = (HCURSOR)LoadImage(NULL, MAKEINTRESOURCE(OCR_NORMAL), IMAGE_CURSOR, 0, 0,
				LR_DEFAULTSIZE | LR_SHARED);
		}

		hCursor = Fallback;

		Out.FromFallback = Fallback != NULL;
	}

	if (!hCursor)
	{
		return;
	}

	// The bitmap only changes a few times a minute, so it is only read when the
	// handle changes. A shared system cursor keeps one handle for its lifetime, and
	// these are the system's own cursors.
	//
	// Compared against the handle that is about to be read, the fallback included.
	// Comparing the desktop's own handle instead -- which is what this used to do --
	// means comparing NULL with NULL for as long as the desktop has no cursor, so it
	// returned here on every frame before reaching the fallback above and left the
	// viewer on libvncserver's placeholder until the container produced a cursor of
	// its own.
	static HCURSOR LastCursor = NULL;

	if (hCursor == LastCursor)
	{
		return;
	}

	Out.ShapeRead = true;

	ICONINFO info = { sizeof(info) };
	if (!GetIconInfo(hCursor, &info))
	{
		return;
	}

	// hbmColor is null for the legacy mask-only cursors. Those are rare enough
	// that the viewer just keeps the cursor it already has rather than growing a
	// second render path here.
	if (!info.hbmColor)
	{
		DeleteObject(info.hbmMask);
		return;
	}

	BITMAP bm = { 0 };
	GetObject(info.hbmColor, sizeof(bm), &bm);

	const unsigned long W = (unsigned long)bm.bmWidth;
	const unsigned long H = (unsigned long)bm.bmHeight;

	// Ask GetDIBits for BGRA rather than drawing through DrawIconEx: GDI does
	// not maintain the alpha channel, and a 32bpp cursor's alpha is what makes
	// it composite correctly over the frame.
	BITMAPINFO bmi = { 0 };
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = (LONG)W;
	bmi.bmiHeader.biHeight = -(LONG)H;   // top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	const unsigned long HotX = (unsigned long)info.xHotspot;
	const unsigned long HotY = (unsigned long)info.yHotspot;

	std::vector<unsigned char> Pixels((size_t)W * H * 4);

	HDC hdc = CreateCompatibleDC(NULL);
	const int Got = GetDIBits(hdc, info.hbmColor, 0, H, Pixels.data(), &bmi, DIB_RGB_COLORS);
	DeleteDC(hdc);

	DeleteObject(info.hbmColor);
	DeleteObject(info.hbmMask);

	if (!Got)
	{
		return;
	}

	LastCursor = hCursor;

	Out.ShapeChanged = true;
	Out.Width = W;
	Out.Height = H;
	Out.HotX = HotX;
	Out.HotY = HotY;
	Out.Shape.swap(Pixels);
}

void Desktop::PointerMove(int X, int Y)
{
	// This is what positions the cursor. The dx/dy SendInput takes are ignored,
	// because MOUSEEVENTF_ABSOLUTE is not set.
	SetCursorPos(X, Y);
}

void Desktop::PointerButton(int Button, bool bDown)
{
	INPUT input = { 0 };
	input.type = INPUT_MOUSE;

	switch (Button)
	{
	case 1:
		input.mi.dwFlags = bDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
		break;
	case 2:
		input.mi.dwFlags = bDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
		break;
	case 3:
		input.mi.dwFlags = bDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
		break;
	default:
		return;
	}

	Send(input, "mouse");
}

void Desktop::Wheel(int X, int Y, int Delta, int HDelta)
{
	// The system delivers the wheel to the window with keyboard focus, but the
	// message carries the cursor position, so the cursor has to be where the user
	// was pointing.
	SetCursorPos(X, Y);

	INPUT input = { 0 };
	input.type = INPUT_MOUSE;

	// One notch is WHEEL_DELTA, and the sign carries over from the viewer: positive
	// is away from the user vertically, to the right horizontally. mouseData is a
	// DWORD holding a signed delta, so the cast is intended.
	//
	// Two calls, because one INPUT carries one axis.
	if (Delta)
	{
		input.mi.dwFlags = MOUSEEVENTF_WHEEL;
		input.mi.mouseData = (DWORD)Delta;

		Send(input, "wheel");
	}

	if (HDelta)
	{
		input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
		input.mi.mouseData = (DWORD)HDelta;

		Send(input, "horizontal wheel");
	}
}

void Desktop::Key(int VirtualKey, bool bDown)
{
	INPUT input = { 0 };
	input.type = INPUT_KEYBOARD;
	input.ki.dwFlags = bDown ? 0 : KEYEVENTF_KEYUP;
	input.ki.dwFlags |= KEYEVENTF_SCANCODE;
	input.ki.wVk = (WORD)VirtualKey;

	// The scan code is what actually goes in, and it has to be the one this
	// keyboard layout gives that key. MAPVK_VK_TO_VSC_EX answers in the form the
	// extended-key flag wants: 0xE0 in the high byte for the keys that need it
	// (arrows, Home/End, PgUp/PgDn, Insert/Delete, numpad Enter and Divide, right
	// Ctrl/Alt, Win/Menu). Without the flag the system does not recognise the
	// scan code and drops the key.
	const UINT Scan = MapVirtualKeyW((UINT)VirtualKey, MAPVK_VK_TO_VSC_EX);

	if (!Scan)
	{
		// No scan code on this layout. Inject the virtual key itself, which is
		// all there is for keys that exist only as one.
		input.ki.dwFlags &= ~KEYEVENTF_SCANCODE;
		input.ki.wScan = 0;

		Send(input, "key");

		return;
	}

	if ((Scan >> 8) == 0xE0)
	{
		input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
	}

	input.ki.wScan = (WORD)(Scan & 0xFF);

	Send(input, "key");
}

// --- the clipboard ----------------------------------------------------------
//
// Two encodings meet here: Windows' clipboard is UTF-16 and RFB's cut text is bytes, and
// which bytes a viewer sends is not stated in the message -- UTF-8 in practice, Latin-1
// by the letter of the protocol. The guess made here is the mirror of the one viewers
// make in the other direction.

unsigned long Desktop::ClipboardSequence()
{
	return (unsigned long)GetClipboardSequenceNumber();
}

bool Desktop::ReadClipboardText(std::string& Out)
{
	Out.clear();

	if (!OpenClipboard(NULL))
	{
		// Another process is holding it. Nothing to do but try again next frame.
		return false;
	}

	// Every way out of here closes it, including the ones that fail.
	HANDLE hData = GetClipboardData(CF_UNICODETEXT);

	if (!hData)
	{
		CloseClipboard();

		return false;
	}

	const wchar_t* Text = (const wchar_t*)GlobalLock(hData);

	if (!Text)
	{
		CloseClipboard();

		return false;
	}

	// The length is taken rather than letting WideCharToMultiByte count to the
	// terminator, because the terminator is not wanted in the output.
	const int Length = (int)wcslen(Text);

	if (Length > 0)
	{
		const int Bytes = WideCharToMultiByte(CP_UTF8, 0, Text, Length, NULL, 0, NULL, NULL);

		if (Bytes > 0)
		{
			Out.resize((size_t)Bytes);

			WideCharToMultiByte(CP_UTF8, 0, Text, Length, &Out[0], Bytes, NULL, NULL);
		}
	}

	GlobalUnlock(hData);
	CloseClipboard();

	return true;
}

bool Desktop::SetClipboardText(const std::string& Utf8)
{
	std::wstring Wide;

	if (!Utf8.empty())
	{
		// MB_ERR_INVALID_CHARS is what makes the first attempt a test rather than a
		// conversion: it fails outright on anything that is not valid UTF-8, which is
		// the signal to fall back.
		const int Chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			Utf8.data(), (int)Utf8.size(), NULL, 0);

		if (Chars > 0)
		{
			Wide.resize((size_t)Chars);

			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
				Utf8.data(), (int)Utf8.size(), &Wide[0], Chars);
		}
		else
		{
			// Latin-1: one byte per character, same value. Not CP_ACP, which would
			// depend on the container's code page and turn the same bytes into
			// something else.
			Wide.reserve(Utf8.size());

			for (const char Byte : Utf8)
			{
				Wide += (wchar_t)(unsigned char)Byte;
			}
		}
	}

	if (!OpenClipboard(NULL))
	{
		return false;
	}

	// Required before SetClipboardData: it is what gives this task ownership.
	if (!EmptyClipboard())
	{
		CloseClipboard();

		return false;
	}

	// Allocated moveable because the clipboard takes ownership of it and will free it
	// itself. From the moment SetClipboardData succeeds nothing here may free it,
	// which is why the failure path below is the only one that does.
	const size_t Bytes = (Wide.size() + 1) * sizeof(wchar_t);
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, Bytes);

	if (!hMem)
	{
		CloseClipboard();

		return false;
	}

	void* p = GlobalLock(hMem);

	if (!p)
	{
		GlobalFree(hMem);
		CloseClipboard();

		return false;
	}

	memcpy(p, Wide.c_str(), Bytes);

	GlobalUnlock(hMem);

	if (!SetClipboardData(CF_UNICODETEXT, hMem))
	{
		GlobalFree(hMem);
		CloseClipboard();

		return false;
	}

	CloseClipboard();

	return true;
}
