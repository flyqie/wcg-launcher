# zlib and libjpeg-turbo, from the submodules in deps/, for the two encodings
# libvncserver only compiles in when it finds them: ZRLE, and Tight. What each is worth
# is in Docs/BUILD.md, section 2.
#
# Submodules rather than something find_package goes looking for, because what
# libvncserver links has to be static and a machine cannot be trusted to have a static
# build of anything. cmake/CheckDependencies.cmake fails the build if a DLL gets in
# anyway.

# The one switch in this project. Off by default, because the default is what this
# program is for: a link that is not loopback, where compressing the bytes is the
# difference between usable and not.
option(LAUNCHER_VNC_PURE
	"Build libvncserver alone: no zlib and no libjpeg-turbo, and so no ZRLE, ZYWRLE \
or Tight" OFF)

if(LAUNCHER_VNC_PURE)
	return()
endif()

foreach(_launcher_dep zlib libjpeg-turbo)
	if(NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/${_launcher_dep}")
		message(FATAL_ERROR
			"deps/${_launcher_dep} is missing. It is a submodule; fetch it with:\n"
			"    git submodule update --init")
	endif()
endforeach()
unset(_launcher_dep)

# --- zlib -------------------------------------------------------------------
#
# A subdirectory, which it is happy to be. Two of its own options have to be set first,
# both about not building what this program cannot use: it builds a shared library and
# its examples by default. Its static library is `zlibstatic` -- `zlib` is the shared
# one, and linking that by mistake is what cmake/CheckDependencies.cmake is for.
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "Set by Launcher; see cmake/Dependencies.cmake" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "Set by Launcher; see cmake/Dependencies.cmake" FORCE)

add_subdirectory("${CMAKE_SOURCE_DIR}/deps/zlib" deps/zlib)

if(NOT TARGET zlibstatic)
	message(FATAL_ERROR
		"The zlib in deps/zlib did not define a `zlibstatic` target. "
		"cmake/Dependencies.cmake is written against zlib 1.3.x; see what the "
		"checkout defines and update it.")
endif()

set(LAUNCHER_ZLIB_INCLUDE_DIRS
	"${CMAKE_SOURCE_DIR}/deps/zlib"
	"${CMAKE_BINARY_DIR}/deps/zlib")

# --- libjpeg-turbo ----------------------------------------------------------
#
# Not a subdirectory: it refuses, because it assumes it is the top-level project --
#
#     "The <project> build system cannot be integrated into another build system
#      using add_subdirectory(). Use ExternalProject_Add() instead."
#
# So it gets a CMake run of its own, which produces a library to point at rather than a
# target to link. It builds statically and picks the static C runtime itself.
set(LAUNCHER_JPEG_BUILD_DIR "${CMAKE_BINARY_DIR}/deps/libjpeg-turbo")

set(LAUNCHER_JPEG_INCLUDE_DIRS
	"${CMAKE_SOURCE_DIR}/deps/libjpeg-turbo/src"
	"${LAUNCHER_JPEG_BUILD_DIR}")

# The imported library is an ExternalProject output rather than a CMake target. Declare
# it explicitly so multi-config generators and incremental builds retain the dependency.
set(LAUNCHER_JPEG_LIBRARY "${LAUNCHER_JPEG_BUILD_DIR}/$<CONFIG>/jpeg-static.lib")

include(ExternalProject)

ExternalProject_Add(LauncherJpeg
	SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/libjpeg-turbo"
	BINARY_DIR "${LAUNCHER_JPEG_BUILD_DIR}"
	CMAKE_ARGS
		-DENABLE_SHARED=OFF
		-DENABLE_STATIC=ON
		-DWITH_SIMD=OFF
		-DWITH_TURBOJPEG=OFF
		-DWITH_TOOLS=OFF
		-DWITH_TESTS=OFF
		-DWITH_JAVA=OFF
	BUILD_BYPRODUCTS "${LAUNCHER_JPEG_LIBRARY}"
	INSTALL_COMMAND "")
