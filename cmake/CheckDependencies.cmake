# Verifies that an executable depends on nothing but the DLLs that come with Windows.
#
# Run after linking, in script mode:
#
#     cmake -DEXE=<the exe> -DLINKER=<link.exe> -P cmake/CheckDependencies.cmake
#
# The container-side program has to be one file, and that is a property of the link
# that is lost silently: a DLL build of zlib reaching the link is all it takes, and
# nothing about the build says so until the container refuses to start. `link /dump
# /dependents` is dumpbin's import table without needing dumpbin on PATH.

if(NOT EXISTS "${EXE}")
	message(FATAL_ERROR "CheckDependencies: no such file: ${EXE}")
endif()

if(NOT LINKER)
	message(FATAL_ERROR "CheckDependencies: LINKER was not set")
endif()

execute_process(
	COMMAND "${LINKER}" /dump /dependents "${EXE}"
	OUTPUT_VARIABLE _dump
	ERROR_VARIABLE _error
	RESULT_VARIABLE _result)

if(NOT _result EQUAL 0)
	message(FATAL_ERROR "CheckDependencies: link /dump failed on ${EXE}:\n${_error}")
endif()

# System32 rather than a list of names, which would be wrong on the next Windows.
if(EXISTS "$ENV{SystemRoot}/System32")
	set(_system "$ENV{SystemRoot}/System32")
else()
	set(_system "C:/Windows/System32")
endif()

string(REPLACE "\r\n" "\n" _dump "${_dump}")
string(REPLACE "\n" ";" _lines "${_dump}")

set(_foreign "")

foreach(_line IN LISTS _lines)
	string(STRIP "${_line}" _name)

	if(NOT _name MATCHES "\\.dll$")
		continue()
	endif()

	if(NOT EXISTS "${_system}/${_name}")
		list(APPEND _foreign "${_name}")
	endif()
endforeach()

if(_foreign)
	list(REMOVE_DUPLICATES _foreign)
	list(SORT _foreign)
	string(REPLACE ";" "\n    " _list "${_foreign}")

	message(FATAL_ERROR
		"${EXE} depends on DLLs that are not part of Windows:\n    ${_list}\n"
		"The container-side program has to be one file, so this build cannot be deployed. Either link that library statically -- deps/ carries both compression libraries as submodules and builds them that way, see cmake/Dependencies.cmake -- or configure with -DLAUNCHER_VNC_PURE=ON and use the encodings libvncserver carries itself. Docs/BUILD.md, section 2.")
endif()
