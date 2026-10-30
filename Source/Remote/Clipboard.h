#pragma once

#include "Rfb.h"

#include <string>

namespace Clipboard
{
	void OnCutText(char* Text, int Length, rfbClientPtr cl);
	void OnCutTextUtf8(char* Text, int Length, rfbClientPtr cl);

	void ApplyText(const std::string& Text);
	void Publish(rfbScreenInfoPtr Screen, unsigned Viewers);

	void Seed();
}
