
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/libvncserver/CMakeLists.txt")
	message(FATAL_ERROR
		"deps/libvncserver is missing. It is a submodule; fetch it with:\n"
		"    git submodule update --init")
endif()

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

foreach(_launcher_vnc_option
		WITH_EXAMPLES WITH_TESTS WITH_QT WITH_SDL WITH_GTK WITH_FFMPEG
		WITH_XCB WITH_SYSTEMD WITH_LIBSSHTUNNEL)
	set(${_launcher_vnc_option} OFF CACHE BOOL "Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)
endforeach()
unset(_launcher_vnc_option)

set(LIBVNCSERVER_INSTALL OFF CACHE BOOL "Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)
set(BUILD_SHARED_LIBS     OFF CACHE BOOL "Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)

if(NOT LAUNCHER_VNC_PURE)
	set(WITH_ZLIB OFF CACHE BOOL "Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)
	set(WITH_JPEG OFF CACHE BOOL "Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)

	set(ZLIB_FOUND TRUE)
	set(ZLIB_INCLUDE_DIR "${LAUNCHER_ZLIB_INCLUDE_DIRS}")
	set(ZLIB_LIBRARIES zlibstatic)

	set(JPEG_FOUND TRUE)
	set(JPEG_INCLUDE_DIR "${LAUNCHER_JPEG_INCLUDE_DIRS}")
	set(JPEG_INCLUDE_DIRS "${LAUNCHER_JPEG_INCLUDE_DIRS}")
	set(JPEG_LIBRARIES "${LAUNCHER_JPEG_LIBRARY}")
else()
	foreach(_launcher_vnc_option WITH_ZLIB WITH_JPEG)
		set(${_launcher_vnc_option} OFF CACHE BOOL
			"Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)
	endforeach()
	unset(_launcher_vnc_option)
endif()

foreach(_launcher_vnc_option WITH_PNG WITH_LZO)
	set(${_launcher_vnc_option} OFF CACHE BOOL
		"Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)
endforeach()

foreach(_launcher_vnc_option WITH_OPENSSL WITH_GNUTLS WITH_GCRYPT WITH_SASL)
	set(${_launcher_vnc_option} OFF CACHE BOOL
		"Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)
endforeach()
unset(_launcher_vnc_option)

add_subdirectory("${CMAKE_SOURCE_DIR}/deps/libvncserver" libvncserver)

foreach(_launcher_vnc_target vncserver vncclient)
	if(TARGET ${_launcher_vnc_target})
		set_property(TARGET ${_launcher_vnc_target} PROPERTY
			MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
	endif()
endforeach()
unset(_launcher_vnc_target)

set(LAUNCHER_LIBVNCSERVER_INCLUDE_DIRS
	"${LibVNCServer_SOURCE_DIR}/include"
	"${LibVNCServer_BINARY_DIR}/include")

if(NOT EXISTS "${LibVNCServer_BINARY_DIR}/include/rfb/rfbconfig.h")
	message(FATAL_ERROR
		"libvncserver did not generate ${LibVNCServer_BINARY_DIR}/include/rfb/rfbconfig.h. "
		"The build would use different feature macros than the library was compiled with.")
endif()

add_library(LauncherVnc INTERFACE)
add_library(Launcher::Vnc ALIAS LauncherVnc)

target_link_libraries(LauncherVnc INTERFACE vncserver)
target_include_directories(LauncherVnc INTERFACE ${LAUNCHER_LIBVNCSERVER_INCLUDE_DIRS})
