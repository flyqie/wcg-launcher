#include "LauncherEnv.h"

#include <Windows.h>

#include <errno.h>
#include <stdlib.h>
#include <string>

#include "LauncherConfig.h"

namespace
{
	std::wstring Read(const wchar_t* Name)
	{
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
	static const int Value = []
	{
		const std::string Text = ReadNarrow("LAUNCHER_VNC_PORT");

		if (!Text.empty())
		{
			char* End = NULL;
			errno = 0;
			const long Port = strtol(Text.c_str(), &End, 10);

			if (End != Text.c_str() && *End == '\0' && errno != ERANGE
				&& Port >= 1024 && Port <= 65535)
			{
				return (int)Port;
			}
		}

		return LAUNCHER_VNC_PORT;
	}();

	return Value;
}

const char* LauncherVncPassword()
{
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

	const wchar_t* WidthText = Value.c_str();
	wchar_t* WidthEnd = NULL;
	wchar_t* HeightEnd = NULL;
	errno = 0;
	const long W = wcstol(WidthText, &WidthEnd, 10);
	const int SavedErrno = errno;
	const wchar_t* HeightText = WidthText + Separator + 1;
	errno = 0;
	const long H = wcstol(HeightText, &HeightEnd, 10);

	if (WidthEnd != WidthText + Separator
		|| HeightEnd != Value.c_str() + Value.size()
		|| SavedErrno == ERANGE || errno == ERANGE)
	{
		return false;
	}

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
	static const bool Enabled = LauncherFlag("LAUNCHER_DOCK");

	return Enabled;
}

int LauncherFps()
{
	static const int Value = []
	{
		const std::string Text = ReadNarrow("LAUNCHER_FPS");

		if (!Text.empty())
		{
			char* End = NULL;
			errno = 0;
			const long Fps = strtol(Text.c_str(), &End, 10);

			if (End != Text.c_str() && *End == '\0' && errno != ERANGE
				&& Fps >= LAUNCHER_FPS_MIN && Fps <= LAUNCHER_FPS_MAX)
			{
				return (int)Fps;
			}
		}

		return LAUNCHER_FPS;
	}();

	return Value;
}
