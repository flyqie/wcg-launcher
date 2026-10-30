#include "Handover.h"

#include <mutex>
#include <string>

#include "Clipboard.h"
#include "Input.h"

namespace
{
	struct Action
	{
		enum Kind { Pointer, Key } What;

		int X = 0;
		int Y = 0;
		int ButtonMask = 0;
		unsigned long KeySym = 0;
		bool bDown = false;
	};

	const unsigned MaxPending = 256;

	struct ActionQueue
	{
		Action Items[MaxPending];
		unsigned Tail = 0;
		unsigned Count = 0;
	};

	ActionQueue gPending;
	bool gResetInput = false;
	std::mutex gPendingLock;

	HANDLE gArrived = NULL;

	std::string gPendingCutText;
	bool gHasPendingCutText = false;

	void Wake()
	{
		if (gArrived)
		{
			SetEvent(gArrived);
		}
	}

	const int PointerWheelBits = 0x08 | 0x10 | 0x20 | 0x40;

	void Post(const Action& Pending)
	{
		{
			std::lock_guard<std::mutex> Lock(gPendingLock);

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
				gPending.Tail = 0;
				gPending.Count = 0;
				gResetInput = true;
			}

			gPending.Items[(gPending.Tail + gPending.Count) % MaxPending] = Pending;
			++gPending.Count;
		}

		Wake();
	}

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
		std::lock_guard<std::mutex> Lock(gPendingLock);

		gPendingCutText.assign(Text, (size_t)Length);
		gHasPendingCutText = true;
	}

	Wake();
}

bool Handover::Apply(rfbScreenInfoPtr Screen)
{
	Action Local[MaxPending];
	unsigned Count = 0;
	std::string CutText;
	bool bHaveCutText = false;
	bool bResetInput = false;

	{
		std::lock_guard<std::mutex> Lock(gPendingLock);

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
