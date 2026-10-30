#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace Desktop
{
	bool Open();

	bool StartProgram(const wchar_t* Command);

	HANDLE ProgramProcess();

	bool ScreenSize(int& Width, int& Height);

	bool HasVisibleWindows();

	struct Frame
	{
		HDC Dc = NULL;
		HGDIOBJ Previous = NULL;
		const unsigned char* Bits = nullptr;
		int Stride = 0;
		int Width = 0;
		int Height = 0;
	};

	bool Capture(Frame& Out);

	void Finish(const Frame& In);

	struct Cursor
	{
		long X = 0;
		long Y = 0;
		bool Visible = false;
		bool ShapeChanged = false;

		unsigned long Width = 0;
		unsigned long Height = 0;
		unsigned long HotX = 0;
		unsigned long HotY = 0;
		std::vector<unsigned char> Shape;

		bool FromFallback = false;

		bool FromWindowClass = false;

		bool HandleMissing = false;
		bool ShapeRead = false;
	};

	void ReadCursor(Cursor& Out);

	void PointerMove(int X, int Y);
	void PointerButton(int Button, bool bDown);
	void Wheel(int X, int Y, int Delta, int HDelta);

	void Key(int VirtualKey, bool bDown);

	struct CaptureBreakdown
	{
		ULONGLONG BitmapAlloc;
		ULONGLONG Fills;
		ULONGLONG WindowPass;
		ULONGLONG TopLevelPrint;
		ULONGLONG ChildReprint;
		unsigned  TopLevelCalls;
		unsigned  ChildCalls;
		unsigned  ChildRepairs;
		ULONGLONG Total;
	};

	const CaptureBreakdown& LastCaptureBreakdown();
	unsigned LastPrintWindowCalls();

	bool ReadClipboardText(std::string& Utf8, std::string& Latin1);

	bool SetClipboardText(const std::string& Utf8);

	unsigned long ClipboardSequence();
}
