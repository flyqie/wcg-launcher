#pragma once

#include "Rfb.h"

#include <string>

// The clipboard, both ways. RFB calls it cut text and carries bytes; Windows'
// clipboard is UTF-16 and belongs to the container. Desktop converts; this decides
// when anything moves.
namespace Clipboard
{
	// libvncserver's callback. Runs on libvncserver's threads; see Handover.
	void OnCutText(char* Text, int Length, rfbClientPtr cl);

	// The frame loop's.
	void ApplyText(const std::string& Text);
	void Publish(rfbScreenInfoPtr Screen, unsigned Viewers);

	// Marks what is on the clipboard now as already known, so that a viewer connecting
	// is not sent the container's current contents. Called once the server exists.
	void Seed();
}
