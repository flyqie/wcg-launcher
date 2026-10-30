
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
		"Link it statically, or configure with -DLAUNCHER_VNC_PURE=ON.")
endif()

execute_process(
	COMMAND "${LINKER}" /dump /headers "${EXE}"
	OUTPUT_VARIABLE _headers
	ERROR_VARIABLE _header_error
	RESULT_VARIABLE _header_result)

if(NOT _header_result EQUAL 0)
	message(FATAL_ERROR "CheckDependencies: link /dump /headers failed on ${EXE}:\n${_header_error}")
endif()

string(REPLACE "\r\n" "\n" _headers "${_headers}")

if(_headers MATCHES "[ \t]+([0-9]+) \\[[ \t]*[0-9]+\\] RVA \\[size\\] of Delay Import Directory")
	if(NOT CMAKE_MATCH_1 EQUAL 0)
		message(FATAL_ERROR
			"${EXE} has a delay-load import table (RVA ${CMAKE_MATCH_1}).\n"
			"Those DLLs are invisible to the check above. Stop delay-loading them.")
	endif()
endif()
