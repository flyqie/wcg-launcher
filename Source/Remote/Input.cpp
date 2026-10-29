#include "Input.h"

#include "../Desktop.h"
#include "../Dock.h"
#include "Handover.h"

using namespace std;

namespace
{
	// --- state, all of it the frame loop's ----------------------------------

	// Where the viewer's pointer last was, in frame coordinates, and whether that was over
	// the dock. The wheel needs the position: an RFB wheel event carries only a change in
	// the button mask.
	int gPointerX = 0;
	int gPointerY = 0;
	bool gPointerOnDock = false;

	// The buttons the viewer is holding, as the RFB mask, and whether the dock swallowed
	// the press that is currently down.
	int gButtonMask = 0;
	bool gDockPress = false;

	// Modifiers, as the bits VkKeyScanW reports: 1 Shift, 2 Control, 4 Alt. gHeld is what
	// the viewer is holding; gTyped is what was pressed here to type a character the
	// viewer asked for without holding the modifier itself, released on key-up.
	int gHeld = 0;
	int gTyped = 0;
	bool gKeysDown[256] = {};

	void InjectKey(int VirtualKey, bool bDown)
	{
		Desktop::Key(VirtualKey, bDown);
		gKeysDown[VirtualKey] = bDown;
	}

	// --- the keyboard -------------------------------------------------------

	// X11 keysyms that are not characters, and the virtual key each one is. Only these
	// need a table: character keysyms go through VkKeyScanW, which is what makes the
	// mapping follow the container's keyboard layout rather than ours.
	struct KeySymEntry
	{
		unsigned long KeySym;
		int VirtualKey;
	};

	const KeySymEntry gKeySyms[] =
	{
		{ 0xFF08, VK_BACK },        // BackSpace
		{ 0xFF09, VK_TAB },         // Tab
		{ 0xFF0D, VK_RETURN },      // Return
		{ 0xFF13, VK_PAUSE },       // Pause
		{ 0xFF14, VK_SCROLL },      // Scroll_Lock
		{ 0xFF1B, VK_ESCAPE },      // Escape
		{ 0xFF50, VK_HOME },        // Home
		{ 0xFF51, VK_LEFT },        // Left
		{ 0xFF52, VK_UP },          // Up
		{ 0xFF53, VK_RIGHT },       // Right
		{ 0xFF54, VK_DOWN },        // Down
		{ 0xFF55, VK_PRIOR },       // Prior, Page Up
		{ 0xFF56, VK_NEXT },        // Next, Page Down
		{ 0xFF57, VK_END },         // End
		{ 0xFF61, VK_SNAPSHOT },    // Print
		{ 0xFF63, VK_INSERT },      // Insert
		{ 0xFF67, VK_APPS },        // Menu
		{ 0xFF7F, VK_NUMLOCK },     // Num_Lock
		{ 0xFF8D, VK_RETURN },      // KP_Enter
		{ 0xFFAA, VK_MULTIPLY },    // KP_Multiply
		{ 0xFFAB, VK_ADD },         // KP_Add
		{ 0xFFAC, VK_SEPARATOR },   // KP_Separator
		{ 0xFFAD, VK_SUBTRACT },    // KP_Subtract
		{ 0xFFAE, VK_DECIMAL },     // KP_Decimal
		{ 0xFFAF, VK_DIVIDE },      // KP_Divide
		{ 0xFFBE, VK_F1 },          // F1
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
		{ 0xFFE1, VK_SHIFT },       // Shift_L
		{ 0xFFE2, VK_SHIFT },       // Shift_R
		{ 0xFFE3, VK_CONTROL },     // Control_L
		{ 0xFFE4, VK_CONTROL },     // Control_R
		{ 0xFFE5, VK_CAPITAL },     // Caps_Lock
		{ 0xFFE7, VK_LWIN },        // Meta_L
		{ 0xFFE8, VK_RWIN },        // Meta_R
		{ 0xFFE9, VK_MENU },        // Alt_L
		{ 0xFFEA, VK_MENU },        // Alt_R
		{ 0xFFEB, VK_LWIN },        // Super_L
		{ 0xFFEC, VK_RWIN },        // Super_R
		{ 0xFE03, VK_RMENU },       // ISO_Level3_Shift, AltGr
		{ 0xFFFF, VK_DELETE },      // Delete
	};

	// Resolves a keysym to a virtual key and the modifiers its key needs. False for a
	// keysym that is not a key on this layout -- a dead key, a character the container's
	// keyboard cannot produce -- for which nothing is injected.
	bool ResolveKeySym(unsigned long KeySym, int& VirtualKey, int& Modifiers)
	{
		// Keypad digits, which are one contiguous run in both encodings.
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

		// A character. VkKeyScanW answers with the virtual key and the modifiers its key
		// needs on this layout, which is the pair wanted here.
		const SHORT Result = VkKeyScanW((WCHAR)KeySym);

		if (Result == -1)
		{
			return false;
		}

		VirtualKey = Result & 0xFF;
		Modifiers = (Result >> 8) & 0xFF;

		return true;
	}

	// Presses or releases one modifier key.
	void SetModifier(int Bit, bool bDown)
	{
		const int VirtualKey = (Bit == 1) ? VK_SHIFT : (Bit == 2) ? VK_CONTROL : VK_MENU;

		InjectKey(VirtualKey, bDown);
	}
}

// Types one key event into the container. The frame loop's.
//
// RFB carries characters, not keys: Shift+A in a viewer sends the keysym for "A" and a
// viewer may never send Shift itself. Mapping that keysym to a key and injecting it on
// its own would type a lowercase "a", so a character whose key needs a modifier is
// typed by pressing that modifier here and releasing it again, unless the viewer is
// already holding it.
void Input::ApplyKey(unsigned long KeySym, bool bDown)
{
	int VirtualKey = 0;
	int Modifiers = 0;

	if (!ResolveKeySym(KeySym, VirtualKey, Modifiers))
	{
		return;
	}

	// A modifier the viewer sent itself is never pressed twice: the second press would be
	// undone by the first release and the rest of the typing would come out unmodified.
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

		// Accumulated rather than assigned, so a press arriving before the previous key was
		// released does not lose the record of a modifier that is down.
		gTyped |= Needed;
	}

	InjectKey(VirtualKey, bDown);

	if (!bDown && gTyped)
	{
		// Released on the first key-up of any kind, which can be early if two keys are held
		// at once. Early is the right way to be wrong: a modifier left down sticks, and
		// everything typed afterwards is wrong until the viewer sends it again.
		if (gTyped & 1) { SetModifier(1, false); }
		if (gTyped & 2) { SetModifier(2, false); }
		if (gTyped & 4) { SetModifier(4, false); }

		gTyped = 0;
	}
}

// Applies one pointer event: motion, a button, or a wheel notch. The frame loop's.
void Input::ApplyPointer(rfbScreenInfoPtr Screen, int ButtonMask, int X, int Y)
{
	gPointerX = X;
	gPointerY = Y;
	gPointerOnDock = Dock::Contains(Y, Screen->height);

	// Buttons 1 to 3: the mask's low bits, which line up with Desktop::PointerButton.
	for (int Bit = 0; Bit < 3; ++Bit)
	{
		const bool bWas = (gButtonMask & (1 << Bit)) != 0;
		const bool bNow = (ButtonMask & (1 << Bit)) != 0;

		if (bWas == bNow)
		{
			continue;
		}

		const int Button = Bit + 1;

		// The dock is the client's own, so anything landing on it is handled here and never
		// reaches the container. Left button only; the others do nothing down there.
		if (gPointerOnDock)
		{
			if (Button != 1)
			{
				continue;
			}

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
				// Pressed on the desktop, released over the dock -- dragging a window past
				// the bottom edge does this. The container is still holding the button down,
				// so the release has to reach it or the drag never ends.
				Desktop::PointerButton(1, false);
			}

			continue;
		}

		Desktop::PointerButton(Button, bNow);
	}

	// The wheel: four more bits of the same mask, each set for one event and cleared by
	// the next. One notch is WHEEL_DELTA, and the sign is RFB's -- up and right positive,
	// which is the way Desktop::Wheel counts. Nothing in the dock scrolls, so a scroll
	// down there is dropped.
	const int WheelBits = 0x08 | 0x10 | 0x20 | 0x40;
	const int Pressed = ButtonMask & ~gButtonMask & WheelBits;

	if (Pressed && !gPointerOnDock)
	{
		int Delta = 0;
		int HDelta = 0;

		if (Pressed & 0x08) { Delta += WHEEL_DELTA; }    // button 4, up
		if (Pressed & 0x10) { Delta -= WHEEL_DELTA; }    // button 5, down
		if (Pressed & 0x20) { HDelta -= WHEEL_DELTA; }   // button 6, left
		if (Pressed & 0x40) { HDelta += WHEEL_DELTA; }   // button 7, right

		Desktop::Wheel(X, Y, Delta, HDelta);
	}

	// Not while over the dock: moving the container's cursor down there would put it
	// behind the dock and leave it there.
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

// libvncserver's keyboard callback.
void Input::OnKeyboard(rfbBool bDown, rfbKeySym KeySym, rfbClientPtr cl)
{
	(void)cl;

	Handover::PostKey((unsigned long)KeySym, bDown != 0);
}

// libvncserver's pointer callback.
//
// rfbDefaultPtrAddEvent is libvncserver's own bookkeeping -- it records where the
// pointer is so the cursor position can be sent on -- and doing it here rather than a
// frame later is what makes the cursor a viewer draws follow its own mouse with no
// added latency. The container's cursor is a frame behind; the one under the user's
// hand is not.
void Input::OnPointer(int ButtonMask, int X, int Y, rfbClientPtr cl)
{
	rfbDefaultPtrAddEvent(ButtonMask, X, Y, cl);

	Handover::PostPointer(ButtonMask, X, Y);
}
