#include "Desktop.h"

#include <Windows.h>

#include <iostream>
#include <vector>

#include "Common/LauncherLog.h"

using namespace std;

// The capture half of the desktop: one pass over every window, painted into a single
// bitmap.
//
// WM_PRINT is the only way to get pixels in a container. There is no DWM, so there is no
// composited screen surface for BitBlt to read from a window's DC, and no D3D device can
// be created instead -- the container has no adapter and, on the older image, no WARP
// either. See Docs/IMAGE-ANALYSIS.md.
//
// Nothing here knows about the dock or about the viewer.

namespace
{
	// Windows' WM_PRINT reaches children through its PRF_CHILDREN pass, and that
	// pass does not reliably draw themed common controls -- a status bar comes out
	// with its background but no parts. Every visible descendant is therefore
	// re-printed explicitly on top.
	typedef struct {
		int ScreenWidth;
		int ScreenHeight;
		HDC hdcMemory;

		// The scratch DC every window is painted into: one for the whole capture rather
		// than one per window, since GetDCEx on another process's window and
		// CreateCompatibleDC are both real costs.
		HDC hdcScratch;
	} EnumWindowParam;

	// One scratch bitmap, shared by every window and every frame, used to paint a
	// single window before it is copied into the target.
	HBITMAP ghTempBitmap = NULL;
	int gTempBitmapWidth = 0;
	int gTempBitmapHeight = 0;

	// The full-screen target, kept across frames. A DIB section rather than a
	// CreateCompatibleBitmap, because its bits are directly addressable: the frame can be
	// compared against the previous one and copied into the RFB framebuffer without the
	// two full-screen copies a device-dependent bitmap would need -- measured, 2.4-3.7 ms
	// of an 8.6 ms idle frame. Reused until the screen size changes.
	HBITMAP gTargetBitmap = NULL;
	void* gTargetBits = NULL;
	int gTargetWidth = 0;
	int gTargetHeight = 0;

	// PrintWindow() calls made since the last Capture reset.
	unsigned gPrintWindowCalls = 0;

	// The desktop this thread is on, which is the container's. Fetched once; it does not
	// change for the life of the process.
	HDESK gDesktop = NULL;

	// Off unless LAUNCHER_DEBUG is set, in which case nothing below reads the clock.
	//
	// Measured, a QueryPerformanceCounter pair plus the conversion costs 31.7 ns, and a
	// frame takes 9 + (one per window) of them -- about 0.3 us idle, or 0.003% of a 33 ms
	// frame. Not a real cost; gated because there is no reason for it not to be.
	const bool gProfile = DebugEnabled();

	Desktop::CaptureBreakdown gBreakdown = { 0 };
	LARGE_INTEGER gQpcFrequency = { 0 };

	// The two halves of the window pass, folded into gBreakdown when it ends.
	ULONGLONG gTopLevelPrintUs = 0;
	ULONGLONG gChildReprintUs = 0;
	unsigned gTopLevelCalls = 0;
	unsigned gChildCalls = 0;

	// Starts a timer, or leaves it zero when profiling is off.
	void Begin(LARGE_INTEGER& Start)
	{
		if (gProfile)
		{
			QueryPerformanceCounter(&Start);
		}
	}

	// Microseconds since Begin, or 0 when profiling is off.
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

	// Paints hwnd into hdc and counts the call.
	//
	// Sending WM_PRINT directly instead, to avoid the PRF_CHILDREN recursion, was tried
	// and does not work: an HDC is only meaningful in the calling process, so the frame
	// came back empty. See Docs/PERFORMANCE.md, experiment B.
	void PaintWindow(HWND hwnd, HDC hdc)
	{
		PrintWindow(hwnd, hdc, 0);

		++gPrintWindowCalls;
	}

	// Re-prints every descendant of hwnd into hdc. See the note on EnumWindowParam.
	//
	// GetWindow(GW_CHILD)/GW_HWNDNEXT walks from the topmost child down, so the children
	// are iterated in reverse to leave the topmost painted last. OriginX/OriginY are the
	// screen coordinates of hdc's origin.
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
			GetWindowRect(*it, &r);

			POINT OldOrigin;
			SetViewportOrgEx(hdc, r.left - OriginX, r.top - OriginY, &OldOrigin);

			LARGE_INTEGER PrintStart;
			Begin(PrintStart);

			PaintWindow(*it, hdc);
			++gChildCalls;

			gChildReprintUs += Since(PrintStart);

			PrintWindowTree(*it, hdc, OriginX, OriginY);

			SetViewportOrgEx(hdc, OldOrigin.x, OldOrigin.y, NULL);
		}
	}

	BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
	{
		EnumWindowParam* Param = (EnumWindowParam*)lParam;
		WINDOWINFO WindowInfo;

		GetWindowInfo(hwnd, &WindowInfo);

		// A minimized window has no pixels to paint, and it is not lost by being
		// skipped: the dock keeps a button for it. See Dock.h.
		if (!(WindowInfo.dwStyle & WS_VISIBLE) || IsIconic(hwnd))
		{
			return TRUE;
		}

		RECT r;
		GetWindowRect(hwnd, &r);

		// Where the window's rect starts, which is not where it is drawn.
		//
		// A maximized window's rect runs past the screen by the width of the
		// invisible resize border Windows gives it, so GetWindowRect returns a
		// rect larger than the display -- (-8, -8, 1032, 750) for a 1024x768
		// screen. That is what the descendants below are positioned against, so
		// it is kept.
		const int OriginX = r.left;
		const int OriginY = r.top;

		// What is actually drawn has to be clamped to the screen.
		//
		// BitBlt clips its destination, so a window drawn at a negative origin
		// loses the matching strip at the far edge: the copy runs off the target
		// and the last few columns are never written, leaving bare desktop where
		// the window's right edge should be. It is the same at the bottom.
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

		// ghTempBitmap is shared by every window and every frame, and no capture
		// pass is obliged to paint all of it. Fill with the desktop colour first
		// so anything left unpainted blends into the background instead of
		// showing the previously captured window's pixels.
		//
		// Only the part that will be copied out, which starts where the window's
		// rect does rather than at the scratch's own corner.
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

	// Returns the full-screen DIB, rebuilding it only when the size changes, and
	// hands back a pointer to its bits.
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
		Info.bmiHeader.biHeight = -Height;   // top-down, so the bits start at the top row
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
	// Reset on entry so the caller can read this frame's count once we return.
	gPrintWindowCalls = 0;
	gTopLevelPrintUs = 0;
	gChildReprintUs = 0;
	gTopLevelCalls = 0;
	gChildCalls = 0;

	if (gProfile && !gQpcFrequency.QuadPart)
	{
		QueryPerformanceFrequency(&gQpcFrequency);
	}

	LARGE_INTEGER TotalStart;
	Begin(TotalStart);

	HDC hdcSource = GetDC(NULL);

	if (!hdcSource)
	{
		Debug() << "DC could not be retrieved" << endl;
		return false;
	}

	int capX = GetDeviceCaps(hdcSource, HORZRES);
	int capY = GetDeviceCaps(hdcSource, VERTRES);

	HDC hdcMemory = CreateCompatibleDC(hdcSource);

	// One scratch bitmap shared by every window and every frame, so it is not
	// reallocated per window. Rebuilt only when the screen size changes, which it
	// can: see LAUNCHER_RESOLUTION.
	//
	// Bigger than the screen by the widest resize border the system hands out,
	// and it has to be. A maximized window's rect runs past the screen by that
	// much on every side, and PrintWindow paints the window into the DC by its
	// rect, so the part that is actually on screen starts at the border offset
	// rather than at the DC's corner. A scratch the size of the screen clips the
	// far edge away before the copy out, which is what left a strip of bare
	// desktop down the right of a maximized window.
	const int Border = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
	const int ScratchWidth = capX + Border * 2;
	const int ScratchHeight = capY + Border * 2;

	if (!ghTempBitmap || gTempBitmapWidth != ScratchWidth || gTempBitmapHeight != ScratchHeight)
	{
		if (ghTempBitmap)
		{
			DeleteObject(ghTempBitmap);
		}

		ghTempBitmap = CreateCompatibleBitmap(hdcSource, ScratchWidth, ScratchHeight);
		gTempBitmapWidth = ScratchWidth;
		gTempBitmapHeight = ScratchHeight;
	}

	// The DC the scratch bitmap lives in, for the whole capture. Created after the
	// bitmap so that the one select below is the only one needed, and released at the
	// end of Capture.
	HDC hdcScratch = CreateCompatibleDC(hdcSource);

	if (!hdcScratch)
	{
		ReleaseDC(NULL, hdcSource);

		return false;
	}

	HGDIOBJ hScratchOld = SelectObject(hdcScratch, ghTempBitmap);

	LARGE_INTEGER AllocStart;
	Begin(AllocStart);

	void* Bits = NULL;
	HBITMAP hBitmap = EnsureTargetBitmap(hdcSource, capX, capY, &Bits);

	gBreakdown.BitmapAlloc = Since(AllocStart);

	if (!hBitmap)
	{
		DeleteDC(hdcMemory);
		ReleaseDC(NULL, hdcSource);

		return false;
	}

	HGDIOBJ hOldTarget = SelectObject(hdcMemory, hBitmap);

	EnumWindowParam EnumParam;
	EnumParam.ScreenWidth = capX;
	EnumParam.ScreenHeight = capY;
	EnumParam.hdcMemory = hdcMemory;
	EnumParam.hdcScratch = hdcScratch;

	// Fill background with the desktop color.
	//
	// FillRect covers the whole DC, so the Rectangle() that used to precede it --
	// which filled the same area with the DC's default white brush -- was
	// overwritten in full and only bought a second full-screen pass.
	LARGE_INTEGER FillStart;
	Begin(FillStart);

	RECT r{ 0, 0, capX, capY };
	FillRect(hdcMemory, &r, GetSysColorBrush(COLOR_DESKTOP));

	gBreakdown.Fills = Since(FillStart);

	std::vector<HWND> hwndList;

	LARGE_INTEGER PassStart;
	Begin(PassStart);

	// Gather the windows first, so they can be drawn in reverse order --
	// otherwise the back windows appear first.
	if (!gDesktop)
	{
		gDesktop = GetThreadDesktop(GetCurrentThreadId());
	}

	EnumDesktopWindows(gDesktop, CollectProc, (LPARAM)&hwndList);

	for (std::vector<HWND>::reverse_iterator rit = hwndList.rbegin(); rit != hwndList.rend(); rit++)
	{
		EnumWindowsProc(*rit, (LPARAM)&EnumParam);
	}

	gBreakdown.WindowPass = Since(PassStart);
	gBreakdown.TopLevelPrint = gTopLevelPrintUs;
	gBreakdown.ChildReprint = gChildReprintUs;
	gBreakdown.TopLevelCalls = gTopLevelCalls;
	gBreakdown.ChildCalls = gChildCalls;

	// The scratch DC and the screen DC are done with. The frame's own DC stays open:
	// the caller draws on the frame before finishing it, which is where the dock goes.
	SelectObject(hdcScratch, hScratchOld);
	DeleteDC(hdcScratch);
	ReleaseDC(NULL, hdcSource);

	Out.Dc = hdcMemory;
	Out.Previous = hOldTarget;
	Out.Bits = (const unsigned char*)Bits;
	Out.Stride = capX * 4;
	Out.Width = capX;
	Out.Height = capY;

	gBreakdown.Total = Since(TotalStart);

	return true;
}

void Desktop::Finish(const Frame& In)
{
	// Deselect before the DC goes away: the bitmap outlives it, and the caller
	// may still be holding Bits.
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
