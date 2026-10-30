#include "Input.h"

#include "../Desktop.h"
#include "../Dock.h"
#include "Handover.h"

namespace
{

	int gPointerX = 0;
	int gPointerY = 0;
	bool gPointerOnDock = false;

	int gButtonMask = 0;
	bool gDockPress = false;

	int gHeld = 0;
	int gTyped = 0;
	bool gKeysDown[256] = {};

	void InjectKey(int VirtualKey, bool bDown)
	{
		Desktop::Key(VirtualKey, bDown);
		gKeysDown[VirtualKey] = bDown;
	}

	struct KeySymEntry
	{
		unsigned long KeySym;
		int VirtualKey;
	};

	const KeySymEntry gKeySyms[] =
	{
		{ 0xFF08, VK_BACK },
		{ 0xFF09, VK_TAB },
		{ 0xFF0D, VK_RETURN },
		{ 0xFF13, VK_PAUSE },
		{ 0xFF14, VK_SCROLL },
		{ 0xFF1B, VK_ESCAPE },
		{ 0xFF50, VK_HOME },
		{ 0xFF51, VK_LEFT },
		{ 0xFF52, VK_UP },
		{ 0xFF53, VK_RIGHT },
		{ 0xFF54, VK_DOWN },
		{ 0xFF55, VK_PRIOR },
		{ 0xFF56, VK_NEXT },
		{ 0xFF57, VK_END },
		{ 0xFF61, VK_SNAPSHOT },
		{ 0xFF63, VK_INSERT },
		{ 0xFF67, VK_APPS },
		{ 0xFF7F, VK_NUMLOCK },
		{ 0xFF8D, VK_RETURN },
		{ 0xFFAA, VK_MULTIPLY },
		{ 0xFFAB, VK_ADD },
		{ 0xFFAC, VK_SEPARATOR },
		{ 0xFFAD, VK_SUBTRACT },
		{ 0xFFAE, VK_DECIMAL },
		{ 0xFFAF, VK_DIVIDE },
		{ 0xFFBE, VK_F1 },
		{ 0xFFBF, VK_F2 },
		{ 0xFFC0, VK_F3 },
		{ 0xFFC1, VK_F4 },
		{ 0xFFC2, VK_F5 },
		{ 0xFFC3, VK_F6 },
		{ 0xFFC4, VK_F7 },
		{ 0xFFC5, VK_F8 },
		{ 0xFFC6, VK_F9 },
		{ 0xFFC7, VK_F10 },
		{ 0xFFC8, VK_F11 },
		{ 0xFFC9, VK_F12 },
		{ 0xFFE1, VK_SHIFT },
		{ 0xFFE2, VK_SHIFT },
		{ 0xFFE3, VK_CONTROL },
		{ 0xFFE4, VK_CONTROL },
		{ 0xFFE5, VK_CAPITAL },
		{ 0xFFE7, VK_LWIN },
		{ 0xFFE8, VK_RWIN },
		{ 0xFFE9, VK_MENU },
		{ 0xFFEA, VK_MENU },
		{ 0xFFEB, VK_LWIN },
		{ 0xFFEC, VK_RWIN },
		{ 0xFE03, VK_RMENU },
		{ 0xFFFF, VK_DELETE },
	};

	bool ResolveKeySym(unsigned long KeySym, int& VirtualKey, int& Modifiers)
	{
		if (KeySym >= 0xFFB0 && KeySym <= 0xFFB9)
		{
			VirtualKey = VK_NUMPAD0 + (int)(KeySym - 0xFFB0);
			Modifiers = 0;

			return true;
		}

		for (const KeySymEntry& Entry : gKeySyms)
		{
			if (Entry.KeySym == KeySym)
			{
				VirtualKey = Entry.VirtualKey;
				Modifiers = 0;

				return true;
			}
		}

		if (KeySym > 0xFF)
		{
			return false;
		}

		const SHORT Result = VkKeyScanW((WCHAR)KeySym);

		if (Result == -1)
		{
			return false;
		}

		VirtualKey = Result & 0xFF;
		Modifiers = (Result >> 8) & 0xFF;

		return true;
	}

	void SetModifier(int Bit, bool bDown)
	{
		const int VirtualKey = (Bit == 1) ? VK_SHIFT : (Bit == 2) ? VK_CONTROL : VK_MENU;

		InjectKey(VirtualKey, bDown);
	}
}

void Input::ApplyKey(unsigned long KeySym, bool bDown)
{
	int VirtualKey = 0;
	int Modifiers = 0;

	if (!ResolveKeySym(KeySym, VirtualKey, Modifiers))
	{
		return;
	}

	switch (VirtualKey)
	{
	case VK_SHIFT:   gHeld = bDown ? (gHeld | 1) : (gHeld & ~1); break;
	case VK_CONTROL: gHeld = bDown ? (gHeld | 2) : (gHeld & ~2); break;
	case VK_MENU:    gHeld = bDown ? (gHeld | 4) : (gHeld & ~4); break;
	default: break;
	}

	if (bDown)
	{
		const int Needed = Modifiers & ~gHeld;

		if (Needed & 1) { SetModifier(1, true); }
		if (Needed & 2) { SetModifier(2, true); }
		if (Needed & 4) { SetModifier(4, true); }

		gTyped |= Needed;
	}

	InjectKey(VirtualKey, bDown);

	if (!bDown && gTyped)
	{
		if (gTyped & 1) { SetModifier(1, false); }
		if (gTyped & 2) { SetModifier(2, false); }
		if (gTyped & 4) { SetModifier(4, false); }

		gTyped = 0;
	}
}

void Input::ApplyPointer(rfbScreenInfoPtr Screen, int ButtonMask, int X, int Y)
{
	gPointerX = X;
	gPointerY = Y;
	gPointerOnDock = Dock::Contains(Y, Screen->height);

	for (int Bit = 0; Bit < 3; ++Bit)
	{
		const bool bWas = (gButtonMask & (1 << Bit)) != 0;
		const bool bNow = (ButtonMask & (1 << Bit)) != 0;

		if (bWas == bNow)
		{
			continue;
		}

		const int Button = Bit + 1;

		if (gPointerOnDock)
		{
			if (Button == 1)
			{
				if (bNow)
				{
					gDockPress = true;

					Dock::Press(X, Y, Screen->width, Screen->height);
				}
				else if (gDockPress)
				{
					gDockPress = false;
				}
				else
				{
					Desktop::PointerButton(1, false);
				}
			}
			else if (Button == 2 && bNow)
			{
				Dock::Close(X, Y, Screen->width, Screen->height);
			}

			continue;
		}

		Desktop::PointerButton(Button, bNow);
	}

	const int WheelBits = 0x08 | 0x10 | 0x20 | 0x40;
	const int Pressed = ButtonMask & ~gButtonMask & WheelBits;

	if (Pressed && !gPointerOnDock)
	{
		int Delta = 0;
		int HDelta = 0;

		if (Pressed & 0x08) { Delta += WHEEL_DELTA; }
		if (Pressed & 0x10) { Delta -= WHEEL_DELTA; }
		if (Pressed & 0x20) { HDelta -= WHEEL_DELTA; }
		if (Pressed & 0x40) { HDelta += WHEEL_DELTA; }

		Desktop::Wheel(X, Y, Delta, HDelta);
	}

	if (!gPointerOnDock)
	{
		Desktop::PointerMove(X, Y);
	}

	gButtonMask = ButtonMask;
}

void Input::ReleaseAll()
{
	for (int Key = 0; Key < 256; ++Key)
	{
		if (gKeysDown[Key]) { InjectKey(Key, false); }
	}

	for (int Bit = 0; Bit < 3; ++Bit)
	{
		if (gButtonMask & (1 << Bit)) { Desktop::PointerButton(Bit + 1, false); }
	}

	gHeld = 0;
	gTyped = 0;
	gButtonMask = 0;
	gDockPress = false;
}

void Input::PointerPosition(int& X, int& Y)
{
	X = gPointerX;
	Y = gPointerY;
}

void Input::OnKeyboard(rfbBool bDown, rfbKeySym KeySym, rfbClientPtr cl)
{
	(void)cl;

	Handover::PostKey((unsigned long)KeySym, bDown != 0);
}

void Input::OnPointer(int ButtonMask, int X, int Y, rfbClientPtr cl)
{
	rfbDefaultPtrAddEvent(ButtonMask, X, Y, cl);

	Handover::PostPointer(ButtonMask, X, Y);
}
