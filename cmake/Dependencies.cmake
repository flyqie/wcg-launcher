
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

set(ZLIB_BUILD_SHARED OFF CACHE BOOL "Set by Launcher; see cmake/Dependencies.cmake" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "Set by Launcher; see cmake/Dependencies.cmake" FORCE)

add_subdirectory("${CMAKE_SOURCE_DIR}/deps/zlib" deps/zlib)

if(NOT TARGET zlibstatic)
	message(FATAL_ERROR "deps/zlib did not define a `zlibstatic` target.")
endif()

set(LAUNCHER_ZLIB_INCLUDE_DIRS
	"${CMAKE_SOURCE_DIR}/deps/zlib"
	"${CMAKE_BINARY_DIR}/deps/zlib")

set(LAUNCHER_JPEG_BUILD_DIR "${CMAKE_BINARY_DIR}/deps/libjpeg-turbo")

set(LAUNCHER_JPEG_INCLUDE_DIRS
	"${CMAKE_SOURCE_DIR}/deps/libjpeg-turbo/src"
	"${LAUNCHER_JPEG_BUILD_DIR}")

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
