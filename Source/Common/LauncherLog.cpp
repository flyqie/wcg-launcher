#include "LauncherLog.h"

#include <Windows.h>

#include <iostream>
#include <streambuf>

#include "LauncherEnv.h"

namespace
{
	// Throws away everything written to it, so the caller's stream expression
	// costs a few virtual calls and no formatting.
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
	// Read once: the frame profiler calls this every frame.
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

	// Every message starts from decimal. std::cout's formatting flags are sticky, so one
	// message that writes `<< hex` to print an error code leaves every later message in
	// hexadecimal, with nothing at the site that did it to say so -- measured the hard
	// way, when a single failed SendInput turned the whole frame profile into hex.
	//
	// Reset here rather than at each of the dozen places that ask for hex, because the
	// ones added later are the ones that would be forgotten.
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
