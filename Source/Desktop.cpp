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

namespace
{

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

	HANDLE gProgramProcess = NULL;

	bool GrantEveryoneAccess()
	{
		HWINSTA hWinSta = OpenWindowStation(TEXT("WinSta0"), FALSE, GENERIC_EXECUTE | WRITE_DAC);

		if (!hWinSta)
		{
			Debug() << "Opening WinSta0 failed (err 0x" << std::hex << GetLastError() << ")" << std::endl;

			return false;
		}

		HWINSTA hPrevious = GetProcessWindowStation();

		if (!SetProcessWindowStation(hWinSta))
		{
			Debug() << "SetProcessWindowStation failed (err 0x" << std::hex << GetLastError() << ")" << std::endl;

			CloseWindowStation(hWinSta);

			return false;
		}

		HDESK hDesktop = OpenDesktop(TEXT("Default"), 0, FALSE, GENERIC_READ | WRITE_DAC);

		if (!hDesktop)
		{
			Debug() << "Opening Default desktop failed (err 0x" << std::hex << GetLastError() << ")" << std::endl;

			SetProcessWindowStation(hPrevious);
			CloseWindowStation(hWinSta);

			return false;
		}

		SID_IDENTIFIER_AUTHORITY SIDAuthWorld = SECURITY_WORLD_SID_AUTHORITY;

		PSID pEveryoneSID = NULL;

		if (!AllocateAndInitializeSid(&SIDAuthWorld, 1, SECURITY_WORLD_RID,
			0, 0, 0, 0, 0, 0, 0, &pEveryoneSID))
		{
			Debug() << "AllocateAndInitializeSid failed (err 0x" << std::hex << GetLastError() << ")" << std::endl;

			SetProcessWindowStation(hPrevious);
			CloseDesktop(hDesktop);
			CloseWindowStation(hWinSta);

			return false;
		}

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

		PACL Dacl = NULL;

		DWORD Status = SetEntriesInAcl(1, &ExplicitAccess, NULL, &Dacl);

		bool bGranted = false;

		if (Status == ERROR_SUCCESS && Dacl)
		{
			HANDLE hToken = NULL;

			if (OpenProcessToken(GetCurrentProcess(),
				TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
			{
				if (AdjustSingleTokenPrivilege(hToken, SE_SECURITY_NAME, SE_PRIVILEGE_ENABLED))
				{
					Status = SetSecurityInfo(hWinSta, SE_KERNEL_OBJECT,
						DACL_SECURITY_INFORMATION, NULL, NULL, Dacl, NULL);

					if (Status == ERROR_SUCCESS)
					{
						Status = SetSecurityInfo(hDesktop, SE_KERNEL_OBJECT,
							DACL_SECURITY_INFORMATION, NULL, NULL, Dacl, NULL);
					}

					if (Status == ERROR_SUCCESS)
					{
						bGranted = true;
					}
					else
					{
						Debug() << "SetSecurityInfo returned " << std::hex << Status << std::endl;
					}

					AdjustSingleTokenPrivilege(hToken, SE_SECURITY_NAME, 0);
				}
				else
				{
					Debug() << "AdjustTokenPrivileges failed (err 0x" << std::hex << GetLastError() << ")" << std::endl;
				}

				CloseHandle(hToken);
			}
		}
		else
		{
			Debug() << "SetEntriesInAcl returned " << std::hex << Status << std::endl;
		}

		if (Dacl)
		{
			LocalFree(Dacl);
		}

		FreeSid(pEveryoneSID);

		SetProcessWindowStation(hPrevious);
		CloseDesktop(hDesktop);
		CloseWindowStation(hWinSta);

		if (bGranted)
		{
			Debug() << "WinSta0 opened to everyone" << std::endl;
		}

		return bGranted;
	}

	bool AssignWindowStation()
	{
		HWINSTA hWinSta = OpenWindowStation(TEXT("WinSta0"), FALSE, GENERIC_ALL);

		if (!hWinSta)
		{
			hWinSta = CreateWindowStation(TEXT("WinSta0"), 0, GENERIC_ALL, NULL);
		}

		if (!hWinSta)
		{
			Debug() << "Opening WinSta0 failed (err 0x" << std::hex << GetLastError() << ")" << std::endl;

			return false;
		}

		if (!SetProcessWindowStation(hWinSta))
		{
			Debug() << "SetProcessWindowStation failed (err 0x" << std::hex << GetLastError() << ")" << std::endl;

			CloseWindowStation(hWinSta);

			return false;
		}

		HDESK hDesktop = OpenDesktop(TEXT("Default"), 0, FALSE, GENERIC_ALL);

		if (!hDesktop)
		{
			hDesktop = CreateDesktop(TEXT("Default"), NULL, NULL, 0, GENERIC_ALL, NULL);
		}

		if (!hDesktop)
		{
			Debug() << "Opening Default desktop failed (err 0x" << std::hex << GetLastError() << ")" << std::endl;

			return false;
		}

		if (!SetThreadDesktop(hDesktop))
		{
			Debug() << "SetThreadDesktop failed (err 0x" << std::hex << GetLastError() << ")" << std::endl;

			return false;
		}

		USEROBJECTFLAGS Flags{ 0 };
		Flags.dwFlags = WSF_VISIBLE;

		if (!SetUserObjectInformation(hWinSta, UOI_FLAGS, &Flags, sizeof(Flags)))
		{
			Debug() << "SetUserObjectInformation failed (err 0x" << std::hex << GetLastError() << ")" << std::endl;
		}

		if (!SwitchDesktop(hDesktop))
		{
			Debug() << "SwitchDesktop failed (err 0x" << std::hex << GetLastError() << ")" << std::endl;
		}

		return true;
	}

	void SetDesktopColour()
	{
		INT Elements[] = { COLOR_DESKTOP };
		COLORREF RgbValues[] = { RGB(8, 110, 165) };

		SetSysColors(1, Elements, RgbValues);
	}

	bool SetDisplayMode(int Width, int Height)
	{
		DEVMODEW Mode;
		memset(&Mode, 0, sizeof(Mode));

		Mode.dmSize = sizeof(Mode);

		Mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
		Mode.dmPelsWidth = (DWORD)Width;
		Mode.dmPelsHeight = (DWORD)Height;
		Mode.dmBitsPerPel = 32;
		Mode.dmDisplayFrequency = 60;

		const LONG Result = ChangeDisplaySettingsW(&Mode, 0);

		if (Result != DISP_CHANGE_SUCCESSFUL)
		{
			Debug() << "ChangeDisplaySettings to " << Width << "x" << Height
			        << " rejected (code " << Result << ")" << std::endl;

			return false;
		}

		Debug() << "Display mode set to " << Width << "x" << Height << std::endl;

		return true;
	}

	void Send(INPUT& Input, const char* What)
	{
		if (!SendInput(1, &Input, sizeof(INPUT)))
		{
			Debug() << "SendInput for " << What << " returned false with error "
			        << std::hex << GetLastError() << std::endl;
		}
	}

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
	const bool bGranted = GrantEveryoneAccess();

	Debug() << "Attaching to the desktop..." << std::endl;

	if (!AssignWindowStation())
	{
		return false;
	}

	if (!bGranted)
	{
		GrantEveryoneAccess();
	}

	SetDesktopColour();

	int ModeWidth = 0;
	int ModeHeight = 0;

	if (LauncherResolution(ModeWidth, ModeHeight))
	{
		SetDisplayMode(ModeWidth, ModeHeight);
	}

	return true;
}

bool Desktop::ScreenSize(int& Width, int& Height)
{
	HDC Screen = GetDC(NULL);

	if (!Screen)
	{
		return false;
	}

	Width = GetDeviceCaps(Screen, HORZRES);
	Height = GetDeviceCaps(Screen, VERTRES);

	ReleaseDC(NULL, Screen);

	return Width > 0 && Height > 0;
}

bool Desktop::StartProgram(const wchar_t* Command)
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

	DebugW() << L"Desktop: " << lpDesktop << std::endl;

	si.lpDesktop = lpDesktop;

	const wchar_t* ToRun = (Command && *Command) ? Command : L"cmd.exe";

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
		Debug() << "Process creation failed: error " << GetLastError() << std::endl;

		return false;
	}

	if (gProgramProcess)
	{
		CloseHandle(gProgramProcess);
	}

	gProgramProcess = pi.hProcess;

	CloseHandle(pi.hThread);

	Debug() << "Waiting for input..." << std::endl;
	WaitForInputIdle(pi.hProcess, INFINITE);

	DWORD dwCount = 0;
	while (!HasVisibleWindows() && dwCount < 10000)
	{
		dwCount += 100;
		Sleep(100);
	}

	Debug() << "Done" << std::endl;

	return true;
}

HANDLE Desktop::ProgramProcess()
{
	return gProgramProcess;
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

	Out.Visible = cursor.flags != CURSOR_SUPPRESSED;

	HCURSOR hCursor = cursor.hCursor;

	Out.HandleMissing = hCursor == NULL;

	if (!hCursor)
	{
		POINT Point = { cursor.ptScreenPos.x, cursor.ptScreenPos.y };
		HWND Under = WindowFromPoint(Point);

		if (Under)
		{
			hCursor = (HCURSOR)GetClassLongPtrW(Under, GCLP_HCURSOR);

			Out.FromWindowClass = hCursor != NULL;
		}
	}

	if (!hCursor)
	{
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

	if (!info.hbmColor)
	{
		DeleteObject(info.hbmMask);
		return;
	}

	BITMAP bm = { 0 };
	GetObject(info.hbmColor, sizeof(bm), &bm);

	const unsigned long W = (unsigned long)bm.bmWidth;
	const unsigned long H = (unsigned long)bm.bmHeight;

	BITMAPINFO bmi = { 0 };
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = (LONG)W;
	bmi.bmiHeader.biHeight = -(LONG)H;
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
	SetCursorPos(X, Y);

	INPUT input = { 0 };
	input.type = INPUT_MOUSE;

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

	const UINT Scan = MapVirtualKeyW((UINT)VirtualKey, MAPVK_VK_TO_VSC_EX);

	if (!Scan)
	{
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

unsigned long Desktop::ClipboardSequence()
{
	return (unsigned long)GetClipboardSequenceNumber();
}

bool Desktop::ReadClipboardText(std::string& Utf8, std::string& Latin1)
{
	Utf8.clear();
	Latin1.clear();

	if (!OpenClipboard(NULL))
	{
		return false;
	}

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

	const int Length = (int)wcslen(Text);

	if (Length > 0)
	{
		const int Bytes = WideCharToMultiByte(CP_UTF8, 0, Text, Length, NULL, 0, NULL, NULL);

		if (Bytes > 0)
		{
			Utf8.resize((size_t)Bytes);

			WideCharToMultiByte(CP_UTF8, 0, Text, Length, &Utf8[0], Bytes, NULL, NULL);
		}

		Latin1.reserve((size_t)Length);

		for (int i = 0; i < Length; ++i)
		{
			Latin1 += (Text[i] <= 0xFF) ? (char)(unsigned char)Text[i] : '?';
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

	if (!EmptyClipboard())
	{
		CloseClipboard();

		return false;
	}

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
