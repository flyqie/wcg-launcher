#include "LauncherEnv.h"

#include <Windows.h>

#include <stdlib.h>
#include <string>

#include "LauncherConfig.h"

namespace
{
	std::wstring Read(const wchar_t* Name)
	{
		// GetEnvironmentVariableW returns 0 both for "not set" and for "set to
		// empty", which are the same thing here.
		const DWORD Length = GetEnvironmentVariableW(Name, NULL, 0);

		if (Length == 0)
		{
			return std::wstring();
		}

		std::wstring Value(Length, L'\0');

		const DWORD Written = GetEnvironmentVariableW(Name, &Value[0], Length);

		if (Written == 0 || Written >= Length)
		{
			return std::wstring();
		}

		Value.resize(Written);

		return Value;
	}

	// The same, for the one setting that is handed to libvncserver as a char*.
	std::string ReadNarrow(const char* Name)
	{
		const DWORD Length = GetEnvironmentVariableA(Name, NULL, 0);

		if (Length == 0)
		{
			return std::string();
		}

		std::string Value(Length, '\0');

		const DWORD Written = GetEnvironmentVariableA(Name, &Value[0], Length);

		if (Written == 0 || Written >= Length)
		{
			return std::string();
		}

		Value.resize(Written);

		return Value;
	}
}

bool LauncherVnc()
{
	return LauncherFlag("LAUNCHER_VNC");
}

int LauncherVncPort()
{
	// Read once: the environment cannot change under a running process.
	static const int Value = []
	{
		const std::string Text = ReadNarrow("LAUNCHER_VNC_PORT");

		if (!Text.empty())
		{
			const int Port = atoi(Text.c_str());

			// Below 1024 is a privileged port the container's account cannot bind, and
			// above 65535 is not a port.
			if (Port >= 1024 && Port <= 65535)
			{
				return Port;
			}
		}

		return LAUNCHER_VNC_PORT;
	}();

	return Value;
}

const char* LauncherVncPassword()
{
	// Read once, and kept for the life of the process: libvncserver holds on to the
	// pointer for as long as the screen does.
	static const std::string Value = ReadNarrow("LAUNCHER_VNC_PW");

	return Value.c_str();
}

bool LauncherFlag(const char* Name)
{
	char Value[8] = { 0 };

	if (!GetEnvironmentVariableA(Name, Value, sizeof(Value)))
	{
		return false;
	}

	return Value[0] == '1' || Value[0] == 'y' || Value[0] == 'Y'
		|| Value[0] == 't' || Value[0] == 'T';
}

bool LauncherResolution(int& Width, int& Height)
{
	static const std::wstring Value = Read(L"LAUNCHER_RESOLUTION");

	if (Value.empty())
	{
		return false;
	}

	const size_t Separator = Value.find_first_of(L"xX*,");

	if (Separator == std::wstring::npos)
	{
		return false;
	}

	const long W = wcstol(Value.c_str(), NULL, 10);
	const long H = wcstol(Value.c_str() + Separator + 1, NULL, 10);

	// A sanity range, not a mode list: what the display driver actually accepts
	// is checked by the driver itself.
	if (W < 100 || H < 100 || W > 16384 || H > 16384)
	{
		return false;
	}

	Width = (int)W;
	Height = (int)H;

	return true;
}

bool LauncherDock()
{
	// Read once: this is asked for on every frame.
	static const bool Enabled = LauncherFlag("LAUNCHER_DOCK");

	return Enabled;
}
