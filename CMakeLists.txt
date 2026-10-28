# Launcher -- the container side.
#
# One program. Launcher.exe builds a window station and desktop inside a
# process-isolated Windows container, paints the desktop into a framebuffer and serves
# it over RFB, so any VNC viewer can see and drive the container's GUI.
#
# Architecture is in README.md; building, running and troubleshooting in Docs/BUILD.md.

cmake_minimum_required(VERSION 3.21)

project(Launcher
	VERSION 1.0
	DESCRIPTION "Serve a Windows container's desktop over VNC"
	LANGUAGES C CXX)

if(NOT WIN32)
	message(FATAL_ERROR "Launcher is a Windows program; there is no other platform.")
endif()

# x64 only: the capture path, the input injection and the single-file deployment all
# assume it.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR
		"Launcher is x64 only. Configure with -A x64 (Visual Studio generator) "
		"or an x64 toolchain (Ninja).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The C standard is left alone: libvncserver sets C_STANDARD 90 on its own targets.

# Static CRT, and load-bearing rather than a preference: the container needs exactly one
# file, so there must be no VC++ redistributable beside it. Set before anything is added,
# which also makes a runtime mismatch with libvncserver a link error rather than a bug.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Inside the source tree on purpose: it is the layout Docs/BUILD.md documents and the one
# its docker command refers to. .gitignore already excludes it.
set(LAUNCHER_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/Output" CACHE PATH
	"Directory the executables are written to")

include(cmake/CompileOptions.cmake)

# The submodules in deps/ first, because libvncserver links them.
include(cmake/Dependencies.cmake)
include(cmake/LibVNCServer.cmake)

add_subdirectory(Source)
