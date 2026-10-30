#include "LauncherLog.h"

#include <Windows.h>

#include <iostream>
#include <streambuf>

#include "LauncherEnv.h"

namespace
{
	class NullBuffer : public std::streambuf
	{
	protected:
		int overflow(int c) override
		{
			return c;
		}
	};

	class NullWBuffer : public std::wstreambuf
	{
	protected:
		wint_t overflow(wint_t c) override
		{
			return c;
		}
	};

	class NullStream : public std::ostream
	{
	public:
		NullStream() : std::ostream(&Buffer) {}

	private:
		NullBuffer Buffer;
	};

	class NullWStream : public std::wostream
	{
	public:
		NullWStream() : std::wostream(&Buffer) {}

	private:
		NullWBuffer Buffer;
	};
}

bool DebugEnabled()
{
	static const bool Enabled = LauncherFlag("LAUNCHER_DEBUG");

	return Enabled;
}

std::ostream& Debug()
{
	static NullStream Null;

	if (!DebugEnabled())
	{
		return Null;
	}

	std::cout.setf(std::ios::dec, std::ios::basefield);

	return std::cout;
}

std::wostream& DebugW()
{
	static NullWStream Null;

	if (!DebugEnabled())
	{
		return Null;
	}

	std::wcout.setf(std::ios::dec, std::ios::basefield);

	return std::wcout;
}
