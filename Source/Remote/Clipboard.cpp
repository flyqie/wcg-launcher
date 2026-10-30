#include "Clipboard.h"

#include <iostream>

#include "../Common/LauncherLog.h"
#include "../Desktop.h"
#include "Handover.h"

namespace
{
	unsigned long gSentSequence = 0;
}

void Clipboard::OnCutText(char* Text, int Length, rfbClientPtr cl)
{
	(void)cl;

	Handover::PostCutText(Text, Length);
}

void Clipboard::OnCutTextUtf8(char* Text, int Length, rfbClientPtr cl)
{
	(void)cl;

	Handover::PostCutText(Text, Length);
}

void Clipboard::ApplyText(const std::string& Text)
{
	if (!Desktop::SetClipboardText(Text))
	{
		return;
	}

	gSentSequence = Desktop::ClipboardSequence();

	Debug() << "Clipboard from a viewer: " << Text.size() << " bytes" << std::endl;
}

void Clipboard::Publish(rfbScreenInfoPtr Screen, unsigned Viewers)
{
	if (!Screen || !Viewers)
	{
		return;
	}

	const unsigned long Sequence = Desktop::ClipboardSequence();

	if (Sequence == gSentSequence)
	{
		return;
	}

	std::string Utf8;
	std::string Latin1;

	if (!Desktop::ReadClipboardText(Utf8, Latin1))
	{
		return;
	}

	gSentSequence = Sequence;

	if (Utf8.empty())
	{
		return;
	}

#ifdef LIBVNCSERVER_HAVE_LIBZ
	rfbSendServerCutTextUTF8(Screen, &Utf8[0], (int)Utf8.size(),
		Latin1.empty() ? NULL : &Latin1[0], (int)Latin1.size());
#else
	rfbSendServerCutText(Screen, &Utf8[0], (int)Utf8.size());
#endif

	Debug() << "Clipboard to the viewers: " << Utf8.size() << " bytes" << std::endl;
}

void Clipboard::Seed()
{
	gSentSequence = Desktop::ClipboardSequence();
}
