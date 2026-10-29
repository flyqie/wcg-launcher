#include "Handover.h"

#include <mutex>
#include <string>

#include "Clipboard.h"
#include "Input.h"

using namespace std;

namespace
{
	// One thing a viewer asked for.
	//
	// A keysym rather than a virtual key, and a button mask rather than a button: turning
	// either into something the desktop understands needs the container's keyboard layout
	// and the dock's hit test, which are questions for the thread that owns the desktop.
	struct Action
	{
		enum Kind { Pointer, Key } What;

		int X = 0;
		int Y = 0;
		int ButtonMask = 0;         // Pointer: the whole mask, so a press and its
		                            //   release stay one thing to be diffed, and
		                            //   so the wheel bits -- which are four more
		                            //   bits of the same mask -- arrive with it
		unsigned long KeySym = 0;   // Key
		bool bDown = false;
	};

	// Deep enough for a frame's worth of input at any human speed, and bounded so a
	// flooding viewer cannot grow it without limit.
	const unsigned MaxPending = 256;

	// A ring, because the two ends move independently: the callbacks write at the head and
	// the frame loop reads from the tail, so neither has to move the other's entries.
	//
	// A std::mutex rather than a CRITICAL_SECTION: this is a namespace-scope object and has
	// to be usable from the moment the first callback can arrive.
	struct ActionQueue
	{
		Action Items[MaxPending];
		unsigned Tail = 0;      // next to be read
		unsigned Count = 0;
	};

	ActionQueue gPending;
	bool gResetInput = false;
	mutex gPendingLock;

	// Set whenever something is posted, and waited on by the frame loop so that input is
	// applied on arrival rather than at the next frame boundary.
	HANDLE gArrived = NULL;

	// Text a viewer has put on its clipboard, waiting for the frame loop. A string rather
	// than an Action because it has no length until it arrives; only the newest is kept,
	// since two viewers pasting at once is a race the protocol has no answer for.
	string gPendingCutText;
	bool gHasPendingCutText = false;

	void Wake()
	{
		if (gArrived)
		{
			SetEvent(gArrived);
		}
	}

	// Wheel events use transient bits in the pointer mask. They must not be coalesced,
	// because two identical wheel masks can represent two separate notches.
	const int PointerWheelBits = 0x08 | 0x10 | 0x20 | 0x40;

	void Post(const Action& Pending)
	{
		{
			lock_guard<mutex> Lock(gPendingLock);

			// Motion without a button transition can replace the previous position.
			if (Pending.What == Action::Pointer && gPending.Count)
			{
				Action& Last = gPending.Items[(gPending.Tail + gPending.Count - 1) % MaxPending];

				if (Last.What == Action::Pointer
					&& Last.ButtonMask == Pending.ButtonMask
					&& !(Pending.ButtonMask & PointerWheelBits))
				{
					Last.X = Pending.X;
					Last.Y = Pending.Y;
					Wake();
					return;
				}
			}

			if (gPending.Count == MaxPending)
			{
				// The batch is no longer lossless. Release what was injected before
				// accepting fresh input, rather than dropping a release and sticking a key.
				gPending.Tail = 0;
				gPending.Count = 0;
				gResetInput = true;
			}

			gPending.Items[(gPending.Tail + gPending.Count) % MaxPending] = Pending;
			++gPending.Count;
		}

		Wake();
	}

	// Takes everything waiting, in order. Returns how many were taken.
	//
	// **The caller holds gPendingLock** -- Apply takes it once, for the actions and the
	// cut text together. Taking it again here deadlocks: std::mutex is not recursive.
	unsigned TakeLocked(Action* Out, unsigned Max)
	{
		const unsigned Taken = gPending.Count < Max ? gPending.Count : Max;

		for (unsigned i = 0; i < Taken; ++i)
		{
			Out[i] = gPending.Items[gPending.Tail];
			gPending.Tail = (gPending.Tail + 1) % MaxPending;
		}

		gPending.Count -= Taken;

		return Taken;
	}
}

HANDLE Handover::Event()
{
	return gArrived;
}

bool Handover::Open()
{
	gArrived = CreateEventW(NULL, TRUE, FALSE, NULL);

	return gArrived != NULL;
}

void Handover::Close()
{
	if (gArrived)
	{
		CloseHandle(gArrived);
		gArrived = NULL;
	}
}

void Handover::PostPointer(int ButtonMask, int X, int Y)
{
	Action Pending;
	Pending.What = Action::Pointer;
	Pending.X = X;
	Pending.Y = Y;
	Pending.ButtonMask = ButtonMask;

	Post(Pending);
}

void Handover::PostKey(unsigned long KeySym, bool bDown)
{
	Action Pending;
	Pending.What = Action::Key;
	Pending.KeySym = KeySym;
	Pending.bDown = bDown;

	Post(Pending);
}

void Handover::PostCutText(const char* Text, int Length)
{
	if (!Text || Length <= 0)
	{
		return;
	}

	{
		lock_guard<mutex> Lock(gPendingLock);

		gPendingCutText.assign(Text, (size_t)Length);
		gHasPendingCutText = true;
	}

	Wake();
}

bool Handover::Apply(rfbScreenInfoPtr Screen)
{
	Action Local[MaxPending];
	unsigned Count = 0;
	string CutText;
	bool bHaveCutText = false;
	bool bResetInput = false;

	{
		lock_guard<mutex> Lock(gPendingLock);

		Count = TakeLocked(Local, MaxPending);
		bResetInput = gResetInput;
		gResetInput = false;

		if (gHasPendingCutText)
		{
			CutText.swap(gPendingCutText);
			gHasPendingCutText = false;
			bHaveCutText = true;
		}
	}

	if (bResetInput)
	{
		Input::ReleaseAll();
	}

	for (unsigned i = 0; i < Count; ++i)
	{
		switch (Local[i].What)
		{
		case Action::Pointer:
			Input::ApplyPointer(Screen, Local[i].ButtonMask, Local[i].X, Local[i].Y);
			break;

		case Action::Key:
			Input::ApplyKey(Local[i].KeySym, Local[i].bDown);
			break;
		}
	}

	if (bHaveCutText)
	{
		Clipboard::ApplyText(CutText);
	}

	return Count != 0 || bHaveCutText || bResetInput;
}
