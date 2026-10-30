#include <Windows.h>
#include <shellapi.h>

#include <string>

#include "Desktop.h"
#include "Dock.h"
#include "Remote/Server.h"
#include "Common/LauncherEnv.h"
#include "Common/LauncherLog.h"

namespace
{
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

	if (!Desktop::Open())
	{
		return 1;
	}

	Dock::Open();

	if (!Desktop::StartProgram(Command.c_str()))
	{
		return 1;
	}

	if (!Server::Run())
	{
		return 1;
	}

	return 0;
}
