#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// The container's desktop: the window station, the screen, the cursor, and the input
// that goes into it -- the equivalent of an X server and a window manager. It would
// still make sense with nothing attached to it, which is the point of the split.
// Nothing here knows about RFB or about a viewer.
namespace Desktop
{
	// Brings up the desktop: opens WinSta0\Default to everyone, attaches this process to
	// it, and applies LAUNCHER_RESOLUTION if it is set. False if the desktop could not be
	// reached, in which case nothing else here is usable.
	bool Open();

	// Starts Command on the desktop; empty means cmd.exe.
	//
	// Separate from Open so that whatever has to be in place first -- the dock's work
	// area, in particular -- can be.
	void StartProgram(const wchar_t* Command);

	// True while the desktop still has a window worth showing. The session has nothing
	// left to do once it does not.
	bool HasVisibleWindows();

	// --- frames -------------------------------------------------------------

	// One screen, captured and open for drawing until it is finished. Valid only between
	// Capture and Finish, and only until the next Capture; what belongs on the picture
	// goes in between, which is where the dock is drawn.
	//
	// Bits is a top-down 32bpp DIB section with no row padding, so it is one contiguous
	// block of Stride * Height bytes and can be read as one.
	struct Frame
	{
		HDC Dc = NULL;              // draw here, in screen coordinates
		HGDIOBJ Previous = NULL;    // what Dc had selected before the frame went in
		const unsigned char* Bits = nullptr;
		int Stride = 0;
		int Width = 0;
		int Height = 0;
	};

	bool Capture(Frame& Out);

	// Closes the frame out: deselects the bitmap and releases the DC.
	void Finish(const Frame& In);

	// --- the cursor ---------------------------------------------------------

	// The container's cursor. Read, never drawn into the frame: RFB carries it separately
	// so that moving it does not dirty the pixels underneath.
	struct Cursor
	{
		long X = 0;                    // screen position of the cursor, not the hotspot
		long Y = 0;
		bool Visible = false;
		bool ShapeChanged = false;     // the bitmap differs from the previous call

		// Only meaningful when ShapeChanged.
		unsigned long Width = 0;
		unsigned long Height = 0;
		unsigned long HotX = 0;
		unsigned long HotY = 0;
		std::vector<unsigned char> Shape;   // 32bpp BGRA, top-down, straight alpha

		// The system arrow standing in for a container that has not shown a cursor yet.
		// Reported rather than kept quiet because "the shape is not changing" and "the
		// shape is the stand-in" look the same from outside.
		bool FromFallback = false;

		// What this call found, for the profile: a cursor that never changes shape has
		// several causes that look identical from outside, and counting them is the only
		// way to tell them apart.
		bool HandleMissing = false;    // the desktop reported no cursor handle
		bool ShapeRead = false;        // the bitmap was actually read this call
	};

	// Written through an out parameter rather than returned by value, which would copy
	// the bitmap on every frame.
	void ReadCursor(Cursor& Out);

	// --- input --------------------------------------------------------------

	// Injected into the desktop. Each one prints why it failed, and only under
	// LAUNCHER_DEBUG.
	void PointerMove(int X, int Y);
	void PointerButton(int Button, bool bDown);       // 1 left, 2 middle, 3 right
	void Wheel(int X, int Y, int Delta, int HDelta);  // notches, positive away and right

	// A virtual key, VK_* as defined in Windows.h. The translation from a keysym is
	// layout-dependent, so it belongs next to the SendInput call rather than a layer away
	// from it; see Remote/Input.h.
	void Key(int VirtualKey, bool bDown);

	// --- profiling ----------------------------------------------------------

	// Where the last capture spent its time, in microseconds, and how many PrintWindow
	// calls it made. The parts are kept apart because they scale with different things:
	// WindowPass with the number of windows and controls, the rest with screen area. See
	// Docs/PERFORMANCE.md.
	struct CaptureBreakdown
	{
		ULONGLONG BitmapAlloc;    // CreateCompatibleBitmap for the full-screen target
		ULONGLONG Fills;          // clearing the target to the desktop colour
		ULONGLONG WindowPass;     // EnumDesktopWindows + every PrintWindow and BitBlt
		ULONGLONG TopLevelPrint;  //   of which: PrintWindow on each top-level window
		ULONGLONG ChildReprint;   //   of which: the extra pass over descendants
		unsigned  TopLevelCalls;  //   of which: how many of each
		unsigned  ChildCalls;
		ULONGLONG Total;          // the whole capture, for cross-checking the parts
	};

	const CaptureBreakdown& LastCaptureBreakdown();
	unsigned LastPrintWindowCalls();

	// --- the clipboard ------------------------------------------------------

	// The container's clipboard as UTF-8, because RFB's cut text is bytes and Windows'
	// clipboard is not. False when it could not be read -- another process holding it, or
	// nothing in it that is text.
	bool ReadClipboardText(std::string& Out);

	// Replaces the container's clipboard. False if it could not be opened, which happens
	// whenever another process is holding it.
	bool SetClipboardText(const std::string& Utf8);

	// The clipboard's change counter. Cheap, needs no window and no message loop, and is
	// the only way to notice from a thread that has neither that the container's
	// clipboard changed.
	unsigned long ClipboardSequence();
}
