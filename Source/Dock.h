#pragma once

#include <Windows.h>

#define LAUNCHER_DOCK_HEIGHT 26

namespace Dock
{
	void Open();

	void Draw(HDC Dc, int Width, int Height, int PointerX, int PointerY);

	bool Contains(int Y, int FrameHeight);

	void Press(int X, int Y, int Width, int FrameHeight);

	void Close(int X, int Y, int Width, int FrameHeight);
}
