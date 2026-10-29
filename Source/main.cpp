#include <Windows.h>
#include <shellapi.h>

#include <string>

#include "Desktop.h"
#include "Dock.h"
#include "Remote/Server.h"
#include "Common/LauncherEnv.h"
#include "Common/LauncherLog.h"

// The container side.
//
//     Launcher.exe [command line]
//
// The optional command line is what gets started on the container's desktop; with none,
// cmd.exe is started. The port comes from LAUNCHER_VNC_PORT and serving at all is opt-in
// through LAUNCHER_VNC -- neither is a command-line argument, because the command line
// belongs to the program being launched.
//
// Three parts, brought up in order: the desktop, then what belongs on top of it, then the
// program, which starts last so that it opens at the right size. Only then does anything
// serve the result. See Desktop.h, Dock.h and Remote/Server.h.

namespace
{
	// Re-quotes one argument so it survives the trip through CreateProcess. Windows
	// treats backslashes immediately before a quote, and trailing backslashes before
	// the closing quote, specially. Doubling those runs is required to preserve the
	// argument after CreateProcess parses the command line again.
	std::wstring Quote(const std::wstring& Argument)
	{
		if (!Argument.empty()
			&& Argument.find_first_of(L" \t\"") == std::wstring::npos)
		{
			return Argument;
		}

		std::wstring Quoted = L"\"";
		size_t Backslashes = 0;

		for (const wchar_t Character : Argument)
		{
			if (Character == L'\\')
			{
				++Backslashes;
				continue;
			}

			if (Character == L'"')
			{
				Quoted.append(Backslashes * 2 + 1, L'\\');
				Quoted += L'"';
				Backslashes = 0;
				continue;
			}

			Quoted.append(Backslashes, L'\\');
			Quoted += Character;
			Backslashes = 0;
		}

		// Backslashes before the closing quote must also be doubled, otherwise the
		// closing quote is interpreted as part of the argument.
		Quoted.append(Backslashes * 2, L'\\');
		Quoted += L'"';

		return Quoted;
	}
}

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;

	int ArgumentCount = 0;
	LPWSTR* Arguments = CommandLineToArgvW(GetCommandLineW(), &ArgumentCount);

	// Everything after the program name is the command line to run on the container's
	// desktop. CommandLineToArgvW has already split and unquoted it, so it is put back
	// together the way CreateProcess wants it.
	std::wstring Command;

	for (int i = 1; Arguments && i < ArgumentCount; ++i)
	{
		if (!Command.empty())
		{
			Command += L' ';
		}

		Command += Quote(Arguments[i]);
	}

	if (Arguments)
	{
		LocalFree(Arguments);
	}

	// Nothing below throws: every failure a caller can act on comes back as a return
	// value, which is what the exit code is made of.
	if (!Desktop::Open())
	{
		return 1;
	}

	// Before the program starts, so it is born knowing how much room it has.
	Dock::Open();

	Desktop::StartProgram(Command.c_str());

	if (!Server::Run())
	{
		return 1;
	}

	return 0;
}
