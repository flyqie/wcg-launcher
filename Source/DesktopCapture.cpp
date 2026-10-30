#include "Desktop.h"

#include <Windows.h>

#include <iostream>
#include <string.h>
#include <vector>

#include "Common/LauncherLog.h"

namespace
{
	struct MemoryDc
	{
		HDC Dc;
		HGDIOBJ Previous = NULL;

		explicit MemoryDc(HDC Source) : Dc(CreateCompatibleDC(Source)) {}
		~MemoryDc()
		{
			if (Dc)
			{
				if (Previous) { SelectObject(Dc, Previous); }
				DeleteDC(Dc);
			}
		}

		bool Select(HBITMAP Bitmap)
		{
			if (!Dc || !Bitmap) { return false; }
			Previous = SelectObject(Dc, Bitmap);
			return Previous && Previous != HGDI_ERROR;
		}

		MemoryDc(const MemoryDc&) = delete;
		MemoryDc& operator=(const MemoryDc&) = delete;
	};

	struct ScreenDc
	{
		HDC Dc = GetDC(NULL);
		~ScreenDc() { if (Dc) { ReleaseDC(NULL, Dc); } }
	};

	typedef struct {
		int ScreenWidth;
		int ScreenHeight;
		HDC hdcMemory;

		HDC hdcScratch;

	} EnumWindowParam;

	HBITMAP ghTempBitmap = NULL;
	unsigned char* gTempBitmapBits = NULL;
	int gTempBitmapWidth = 0;
	int gTempBitmapHeight = 0;
	int gTempBitmapStride = 0;

	HBITMAP gTargetBitmap = NULL;
	void* gTargetBits = NULL;
	int gTargetWidth = 0;
	int gTargetHeight = 0;

	unsigned gPrintWindowCalls = 0;

	HDESK gDesktop = NULL;

	const bool gProfile = DebugEnabled();

	Desktop::CaptureBreakdown gBreakdown = { 0 };
	LARGE_INTEGER gQpcFrequency = { 0 };

	ULONGLONG gTopLevelPrintUs = 0;
	ULONGLONG gChildReprintUs = 0;
	unsigned gTopLevelCalls = 0;
	unsigned gChildCalls = 0;

	void Begin(LARGE_INTEGER& Start)
	{
		if (gProfile)
		{
			QueryPerformanceCounter(&Start);
		}
	}

	ULONGLONG Since(const LARGE_INTEGER& Start)
	{
		if (!gProfile)
		{
			return 0;
		}

		LARGE_INTEGER Now;
		QueryPerformanceCounter(&Now);

		return (ULONGLONG)((Now.QuadPart - Start.QuadPart) * 1000000LL / gQpcFrequency.QuadPart);
	}

	void PaintWindow(HWND hwnd, HDC hdc)
	{
		PrintWindow(hwnd, hdc, 0);

		++gPrintWindowCalls;
	}

	struct ControlPixels
	{
		HWND Window = NULL;
		int Width = 0;
		int Height = 0;
		bool bUniform = true;
		unsigned long long LastSeen = 0;
		std::vector<unsigned char> Pixels;
	};

	std::vector<ControlPixels> gControlCache;

	unsigned long long gFrameCounter = 0;

	unsigned gChildRepairs = 0;

	bool IsUniform(const unsigned char* Pixels, int Width, int Height)
	{
		if (Width <= 0 || Height <= 0)
		{
			return true;
		}

		for (int y = 0; y < Height; ++y)
		{
			const unsigned char* Row = Pixels + (size_t)y * Width * 4;

			for (int x = 0; x < Width; ++x)
			{
				const unsigned char* Pixel = Row + (size_t)x * 4;

				if (Pixel[0] != Pixels[0] || Pixel[1] != Pixels[1] || Pixel[2] != Pixels[2])
				{
					return false;
				}
			}
		}

		return true;
	}

	void RepairChild(HWND Window, int X, int Y, int Width, int Height)
	{
		if (!gTempBitmapBits)
		{
			return;
		}

		int Left = X;
		int Top = Y;
		int Right = X + Width;
		int Bottom = Y + Height;

		if (Left < 0) { Left = 0; }
		if (Top < 0) { Top = 0; }
		if (Right > gTempBitmapWidth) { Right = gTempBitmapWidth; }
		if (Bottom > gTempBitmapHeight) { Bottom = gTempBitmapHeight; }

		const int W = Right - Left;
		const int H = Bottom - Top;

		if (W <= 0 || H <= 0)
		{
			return;
		}

		GdiFlush();

		ControlPixels* Entry = NULL;

		for (ControlPixels& Candidate : gControlCache)
		{
			if (Candidate.Window == Window)
			{
				Entry = &Candidate;
				break;
			}
		}

		if (!Entry)
		{
			gControlCache.push_back(ControlPixels());
			Entry = &gControlCache.back();

			Entry->Window = Window;
		}

		Entry->LastSeen = gFrameCounter;

		if (Entry->Width != W || Entry->Height != H)
		{
			Entry->Width = W;
			Entry->Height = H;
			Entry->Pixels.clear();
			Entry->bUniform = true;
		}

		std::vector<unsigned char> Fresh((size_t)W * H * 4);

		for (int y = 0; y < H; ++y)
		{
			memcpy(&Fresh[(size_t)y * W * 4],
				gTempBitmapBits + (size_t)(Top + y) * gTempBitmapStride + (size_t)Left * 4,
				(size_t)W * 4);
		}

		const bool bFreshUniform = IsUniform(Fresh.data(), W, H);

		if (bFreshUniform && !Entry->bUniform && Entry->Pixels.size() == Fresh.size())
		{
			for (int y = 0; y < H; ++y)
			{
				memcpy(gTempBitmapBits + (size_t)(Top + y) * gTempBitmapStride + (size_t)Left * 4,
					&Entry->Pixels[(size_t)y * W * 4], (size_t)W * 4);
			}

			++gChildRepairs;
		}

		Entry->Pixels.swap(Fresh);
		Entry->bUniform = bFreshUniform;
	}

	void PrintWindowTree(HWND hwnd, HDC hdc, int OriginX, int OriginY)
	{
		std::vector<HWND> Children;

		for (HWND hChild = GetWindow(hwnd, GW_CHILD); hChild; hChild = GetWindow(hChild, GW_HWNDNEXT))
		{
			Children.push_back(hChild);
		}

		for (std::vector<HWND>::reverse_iterator it = Children.rbegin(); it != Children.rend(); ++it)
		{
			if (!IsWindowVisible(*it))
			{
				continue;
			}

			RECT r;
			if (!GetWindowRect(*it, &r)) { continue; }

			POINT OldOrigin;
			SetViewportOrgEx(hdc, r.left - OriginX, r.top - OriginY, &OldOrigin);

			LARGE_INTEGER PrintStart;
			Begin(PrintStart);

			PaintWindow(*it, hdc);
			++gChildCalls;

			gChildReprintUs += Since(PrintStart);

			RepairChild(*it, r.left - OriginX, r.top - OriginY,
				r.right - r.left, r.bottom - r.top);

			PrintWindowTree(*it, hdc, OriginX, OriginY);

			SetViewportOrgEx(hdc, OldOrigin.x, OldOrigin.y, NULL);
		}
	}

	BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
	{
		EnumWindowParam* Param = (EnumWindowParam*)lParam;
		WINDOWINFO WindowInfo = {};
		WindowInfo.cbSize = sizeof(WindowInfo);

		if (!GetWindowInfo(hwnd, &WindowInfo))
		{
			return TRUE;
		}

		if (!(WindowInfo.dwStyle & WS_VISIBLE) || IsIconic(hwnd))
		{
			return TRUE;
		}

		RECT r;
		if (!GetWindowRect(hwnd, &r)) { return TRUE; }

		const int OriginX = r.left;
		const int OriginY = r.top;

		int Left = r.left;
		int Top = r.top;
		int Right = r.right;
		int Bottom = r.bottom;

		if (Left < 0) Left = 0;
		if (Top < 0) Top = 0;
		if (Right > Param->ScreenWidth) Right = Param->ScreenWidth;
		if (Bottom > Param->ScreenHeight) Bottom = Param->ScreenHeight;

		const int x = Left;
		const int y = Top;
		const int w = Right - Left;
		const int h = Bottom - Top;

		if (w <= 0 || h <= 0)
		{
			return TRUE;
		}

		HDC hdcWindowMemory = Param->hdcScratch;

		const int SrcX = x - OriginX;
		const int SrcY = y - OriginY;

		RECT Scratch{ SrcX, SrcY, SrcX + w, SrcY + h };
		FillRect(hdcWindowMemory, &Scratch, GetSysColorBrush(COLOR_DESKTOP));

		LARGE_INTEGER PrintStart;
		Begin(PrintStart);

		PaintWindow(hwnd, hdcWindowMemory);
		++gTopLevelCalls;

		gTopLevelPrintUs += Since(PrintStart);

		PrintWindowTree(hwnd, hdcWindowMemory, OriginX, OriginY);

		BitBlt(Param->hdcMemory, x, y, w, h, hdcWindowMemory, SrcX, SrcY, SRCCOPY);

		return TRUE;
	}

	BOOL CALLBACK CollectProc(HWND hwnd, LPARAM lParam)
	{
		std::vector<HWND>* List = (std::vector<HWND>*)lParam;
		List->push_back(hwnd);

		return TRUE;
	}

	HBITMAP EnsureTargetBitmap(HDC hdcSource, int Width, int Height, void** Bits)
	{
		if (gTargetBitmap && gTargetWidth == Width && gTargetHeight == Height)
		{
			*Bits = gTargetBits;
			return gTargetBitmap;
		}

		if (gTargetBitmap)
		{
			DeleteObject(gTargetBitmap);
			gTargetBitmap = NULL;
			gTargetBits = NULL;
		}

		BITMAPINFO Info = { 0 };
		Info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		Info.bmiHeader.biWidth = Width;
		Info.bmiHeader.biHeight = -Height;
		Info.bmiHeader.biPlanes = 1;
		Info.bmiHeader.biBitCount = 32;
		Info.bmiHeader.biCompression = BI_RGB;

		gTargetBitmap = CreateDIBSection(hdcSource, &Info, DIB_RGB_COLORS, Bits, NULL, 0);

		if (!gTargetBitmap)
		{
			return NULL;
		}

		gTargetBits = *Bits;
		gTargetWidth = Width;
		gTargetHeight = Height;

		return gTargetBitmap;
	}
}

bool Desktop::Capture(Frame& Out)
{
	gPrintWindowCalls = 0;
	gTopLevelPrintUs = 0;
	gChildReprintUs = 0;
	gTopLevelCalls = 0;
	gChildCalls = 0;
	gChildRepairs = 0;

	++gFrameCounter;

	if (gProfile && !gQpcFrequency.QuadPart)
	{
		QueryPerformanceFrequency(&gQpcFrequency);
	}

	LARGE_INTEGER TotalStart;
	Begin(TotalStart);

	ScreenDc Source;
	HDC hdcSource = Source.Dc;

	if (!hdcSource)
	{
		Debug() << "DC could not be retrieved" << std::endl;
		return false;
	}

	int capX = GetDeviceCaps(hdcSource, HORZRES);
	int capY = GetDeviceCaps(hdcSource, VERTRES);

	if (capX <= 0 || capY <= 0) { return false; }

	MemoryDc Target(hdcSource);
	HDC hdcMemory = Target.Dc;
	if (!hdcMemory) { return false; }

	const int Border = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
	const int ScratchWidth = capX + Border * 2;
	const int ScratchHeight = capY + Border * 2;

	if (!ghTempBitmap || gTempBitmapWidth != ScratchWidth || gTempBitmapHeight != ScratchHeight)
	{
		if (ghTempBitmap)
		{
			DeleteObject(ghTempBitmap);
			ghTempBitmap = NULL;
			gTempBitmapBits = NULL;
		}

		BITMAPINFO Info = { 0 };
		Info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		Info.bmiHeader.biWidth = ScratchWidth;
		Info.bmiHeader.biHeight = -ScratchHeight;
		Info.bmiHeader.biPlanes = 1;
		Info.bmiHeader.biBitCount = 32;
		Info.bmiHeader.biCompression = BI_RGB;

		ghTempBitmap = CreateDIBSection(hdcSource, &Info, DIB_RGB_COLORS,
			(void**)&gTempBitmapBits, NULL, 0);

		if (!ghTempBitmap)
		{
			return false;
		}

		gTempBitmapWidth = ScratchWidth;
		gTempBitmapHeight = ScratchHeight;
		gTempBitmapStride = ScratchWidth * 4;
	}

	MemoryDc ScratchDc(hdcSource);
	HDC hdcScratch = ScratchDc.Dc;

	if (!ScratchDc.Select(ghTempBitmap))
	{
		return false;
	}

	LARGE_INTEGER AllocStart;
	Begin(AllocStart);

	void* Bits = NULL;
	HBITMAP hBitmap = EnsureTargetBitmap(hdcSource, capX, capY, &Bits);

	gBreakdown.BitmapAlloc = Since(AllocStart);

	if (!Target.Select(hBitmap))
	{
		return false;
	}

	EnumWindowParam EnumParam;
	EnumParam.ScreenWidth = capX;
	EnumParam.ScreenHeight = capY;
	EnumParam.hdcMemory = hdcMemory;
	EnumParam.hdcScratch = hdcScratch;

	LARGE_INTEGER FillStart;
	Begin(FillStart);

	RECT r{ 0, 0, capX, capY };
	FillRect(hdcMemory, &r, GetSysColorBrush(COLOR_DESKTOP));

	gBreakdown.Fills = Since(FillStart);

	std::vector<HWND> hwndList;

	LARGE_INTEGER PassStart;
	Begin(PassStart);

	if (!gDesktop)
	{
		gDesktop = GetThreadDesktop(GetCurrentThreadId());
	}

	EnumDesktopWindows(gDesktop, CollectProc, (LPARAM)&hwndList);

	if (gProfile)
	{
		for (HWND hwnd : hwndList)
		{
			WCHAR Class[128] = { 0 };
			WCHAR Title[128] = { 0 };
			RECT Bounds = { 0 };

			GetClassNameW(hwnd, Class, 128);
			GetWindowTextW(hwnd, Title, 128);
			GetWindowRect(hwnd, &Bounds);

			DebugW() << L"  window " << Class << L"  \"" << Title << L"\"  "
			         << (Bounds.right - Bounds.left) << L"x" << (Bounds.bottom - Bounds.top)
			         << (IsWindowVisible(hwnd) ? L" visible" : L" hidden")
			         << (IsIconic(hwnd) ? L" iconic" : L"")
			         << std::endl;

		}
	}

	for (std::vector<HWND>::reverse_iterator rit = hwndList.rbegin(); rit != hwndList.rend(); rit++)
	{
		EnumWindowsProc(*rit, (LPARAM)&EnumParam);
	}

	{
		std::vector<ControlPixels> Live;

		for (ControlPixels& Entry : gControlCache)
		{
			if (Entry.LastSeen == gFrameCounter)
			{
				Live.push_back(std::move(Entry));
			}
		}

		gControlCache.swap(Live);
	}

	gBreakdown.WindowPass = Since(PassStart);
	gBreakdown.TopLevelPrint = gTopLevelPrintUs;
	gBreakdown.ChildReprint = gChildReprintUs;
	gBreakdown.TopLevelCalls = gTopLevelCalls;
	gBreakdown.ChildCalls = gChildCalls;
	gBreakdown.ChildRepairs = gChildRepairs;

	Out.Dc = hdcMemory;
	Out.Previous = Target.Previous;
	Target.Dc = NULL;
	Out.Bits = (const unsigned char*)Bits;
	Out.Stride = capX * 4;
	Out.Width = capX;
	Out.Height = capY;

	gBreakdown.Total = Since(TotalStart);

	return true;
}

void Desktop::Finish(const Frame& In)
{
	GdiFlush();

	SelectObject(In.Dc, In.Previous);
	DeleteDC(In.Dc);
}

const Desktop::CaptureBreakdown& Desktop::LastCaptureBreakdown()
{
	return gBreakdown;
}

unsigned Desktop::LastPrintWindowCalls()
{
	return gPrintWindowCalls;
}
