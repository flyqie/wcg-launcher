#include "Clipboard.h"

#include <iostream>

#include "../Common/LauncherLog.h"
#include "../Desktop.h"
#include "Handover.h"

using namespace std;

namespace
{
	// The container's clipboard counter as of the last time the viewers were told. See
	// Publish.
	unsigned long gSentSequence = 0;
}

void Clipboard::OnCutText(char* Text, int Length, rfbClientPtr cl)
{
	(void)cl;

	// Queued like everything else that has to reach the container. The bytes are whatever
	// the viewer chose to send -- UTF-8 in practice, Latin-1 by the letter of the protocol
	// -- and Desktop::SetClipboardText is what tells the two apart.
	Handover::PostCutText(Text, Length);
}

void Clipboard::ApplyText(const string& Text)
{
	if (!Desktop::SetClipboardText(Text))
	{
		// Another process is holding the clipboard. Dropped rather than retried: it came
		// from a viewer that is no longer waiting, and holding it would put a stale paste
		// on the clipboard a second later.
		return;
	}

	// This program is what just changed the clipboard, so the new counter is recorded as
	// already sent. Otherwise it would be read back and sent to the viewer that sent it,
	// which would set that viewer's clipboard, which would send it here again.
	gSentSequence = Desktop::ClipboardSequence();

	Debug() << "Clipboard from a viewer: " << Text.size() << " bytes" << endl;
}

void Clipboard::Publish(rfbScreenInfoPtr Screen, unsigned Viewers)
{
	if (!Screen || !Viewers)
	{
		return;
	}

	// A counter rather than a window and a message loop: GetClipboardSequenceNumber costs
	// nothing and is the only way to notice a change from a thread that has no message
	// queue. The clipboard itself is opened only on frames where the counter moved.
	const unsigned long Sequence = Desktop::ClipboardSequence();

	if (Sequence == gSentSequence)
	{
		return;
	}

	string Text;

	if (!Desktop::ReadClipboardText(Text))
	{
		// Held by another process, or holding something that is not text. The counter is
		// left where it is, so this is tried again next frame.
		return;
	}

	gSentSequence = Sequence;

	if (Text.empty())
	{
		return;
	}

	// Sent as the bytes they are, through the plain path. RFB's cut text has no encoding
	// field, so UTF-8 goes out and the viewer guesses; libvncserver's extended clipboard
	// says so explicitly but is reached through rfbSendServerCutTextUTF8, which this does
	// not use. See Docs/BUILD.md, section 4.6.
	rfbSendServerCutText(Screen, &Text[0], (int)Text.size());

	Debug() << "Clipboard to the viewers: " << Text.size() << " bytes" << endl;
}

void Clipboard::Seed()
{
	gSentSequence = Desktop::ClipboardSequence();
}
