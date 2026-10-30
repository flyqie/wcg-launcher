
cmake_minimum_required(VERSION 3.21)

project(Launcher
	VERSION 1.0
	DESCRIPTION "Serve a Windows container's desktop over VNC"
	LANGUAGES C CXX)

if(NOT WIN32)
	message(FATAL_ERROR "Launcher is a Windows program; there is no other platform.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "Launcher is x64 only. Configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(LAUNCHER_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/Output" CACHE PATH
	"Directory the executables are written to")

include(cmake/CompileOptions.cmake)

include(cmake/Dependencies.cmake)
include(cmake/LibVNCServer.cmake)

set(LAUNCHER_BUILD "" CACHE STRING
	"Version written into Launcher.exe. Empty means no version resource at all, which is what a plain cmake build gets")

string(REPLACE "\"" "" LAUNCHER_BUILD "${LAUNCHER_BUILD}")

set(LAUNCHER_VERSION_BLOCK "")

if(LAUNCHER_BUILD)
	string(CONFIGURE [=[
#include <windows.h>

VS_VERSION_INFO VERSIONINFO
FILEVERSION     1,0,0,0
PRODUCTVERSION  1,0,0,0
FILEFLAGSMASK   VS_FFI_FILEFLAGSMASK
FILEFLAGS       0
FILEOS          VOS_NT_WINDOWS32
FILETYPE        VFT_APP
FILESUBTYPE     VFT2_UNKNOWN
BEGIN
	BLOCK "StringFileInfo"
	BEGIN
		BLOCK "040904b0"
		BEGIN
			VALUE "FileVersion", "@LAUNCHER_BUILD@"
			VALUE "ProductVersion", "@LAUNCHER_BUILD@"
		END
	END
	BLOCK "VarFileInfo"
	BEGIN
		VALUE "Translation", 0x409, 1200
	END
END
]=] LAUNCHER_VERSION_BLOCK @ONLY)
endif()

configure_file(Source/Launcher.rc.in "${CMAKE_BINARY_DIR}/Launcher.rc" @ONLY)

add_subdirectory(Source)
