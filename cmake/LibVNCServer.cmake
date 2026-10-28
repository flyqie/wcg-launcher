# libvncserver, from the submodule in deps/libvncserver/.
#
# Added as a subdirectory rather than found as a package: it is part of this program,
# not a system library it happens to use. Three things have to be arranged around it
# that its own CMakeLists cannot know about, because it is written to be the top-level
# project, and each is marked below:
#
#   1. It asks for CMake 3.4, which CMake 4 refuses outright.
#   2. It exports no usage requirements, so the LauncherVnc interface target carries
#      its include directories.
#   3. It does not select an MSVC runtime library, and the default is /MD.
#
# How it gets the compression libraries is a fourth, and that one is in the block below.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/libvncserver/CMakeLists.txt")
	message(FATAL_ERROR
		"deps/libvncserver is missing. It is a submodule; fetch it with:\n"
		"    git submodule update --init")
endif()

# 1. The CMake floor. A normal variable, not a cache entry: it is this file's business.
# Without it the configure stops at libvncserver's first line.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Set before it is added, because option() ignores a value set afterwards. The examples,
# the Qt client and the optional transports are off: none is used here, and several would
# drag in a dependency the container would then have to carry.
foreach(_launcher_vnc_option
		WITH_EXAMPLES WITH_TESTS WITH_QT WITH_SDL WITH_GTK WITH_FFMPEG
		WITH_XCB WITH_SYSTEMD WITH_LIBSSHTUNNEL)
	set(${_launcher_vnc_option} OFF CACHE BOOL "Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)
endforeach()
unset(_launcher_vnc_option)

set(LIBVNCSERVER_INSTALL OFF CACHE BOOL "Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)
set(BUILD_SHARED_LIBS     OFF CACHE BOOL "Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)

# The compression libraries, which decide which encodings exist.
#
# libvncserver reaches for them through find_package, which looks for a header and a
# library on the machine rather than a target in this build -- and a machine cannot
# be trusted to have a static build of anything. Since deps/ already carries both as
# submodules, built by cmake/Dependencies.cmake, the search is turned off and the
# answers are supplied instead: WITH_ZLIB and WITH_JPEG off stop find_package from
# running at all, and the variables it would have set are set here. That is also
# what keeps a static build of each -- there is nothing to find, and therefore
# nothing to find wrong.
#
# What each one buys is in cmake/Dependencies.cmake; what it is worth on a given
# link is in Docs/BUILD.md, section 2.
#
# LAUNCHER_VNC_PURE is the other half of this: cmake/Dependencies.cmake never runs,
# so there is nothing to supply, and all that is left to do is make sure
# find_package does not go looking on the machine instead.
if(NOT LAUNCHER_VNC_PURE)
	set(WITH_ZLIB OFF CACHE BOOL "Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)
	set(WITH_JPEG OFF CACHE BOOL "Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)

	# libvncserver reads ZLIB_INCLUDE_DIR and JPEG_INCLUDE_DIR for include_directories,
	# and ZLIB_LIBRARIES and JPEG_LIBRARIES for target_link_libraries -- where a target
	# name is what is wanted, because the submodules have already defined them.
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

# Off with no switch, because neither is worth a dependency. libpng adds only TightPng,
# which almost nothing asks for, and the real LZO rather than the miniLZO libvncserver
# carries changes nothing: Ultra exists either way.
foreach(_launcher_vnc_option WITH_PNG WITH_LZO)
	set(${_launcher_vnc_option} OFF CACHE BOOL
		"Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)
endforeach()

# TLS and the crypto backends behind it: off, with no switch to turn them on. The link is
# a container's own desktop to whoever is watching it, and a deployment that wants TLS
# wants something other than this program in front of it. A switch would only buy a build
# that quietly started carrying OpenSSL.
foreach(_launcher_vnc_option WITH_OPENSSL WITH_GNUTLS WITH_GCRYPT WITH_SASL)
	set(${_launcher_vnc_option} OFF CACHE BOOL
		"Set by Launcher; see cmake/LibVNCServer.cmake" FORCE)
endforeach()
unset(_launcher_vnc_option)

add_subdirectory("${CMAKE_SOURCE_DIR}/deps/libvncserver" libvncserver)

# 3. The runtime library, on libvncserver's targets rather than inherited, so a Debug
# build of the executable links against a Debug library.
foreach(_launcher_vnc_target vncserver vncclient)
	if(TARGET ${_launcher_vnc_target})
		set_property(TARGET ${_launcher_vnc_target} PROPERTY
			MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
	endif()
endforeach()
unset(_launcher_vnc_target)

# 2. What to link to get the server. The checkout's include/, and the build tree's, which
# is where configure_file writes the rfbconfig.h that every libvncserver header reads its
# feature macros from. Getting the second one wrong is not a missing header but a silently
# different build, so it is checked rather than assumed.
set(LAUNCHER_LIBVNCSERVER_INCLUDE_DIRS
	"${LibVNCServer_SOURCE_DIR}/include"
	"${LibVNCServer_BINARY_DIR}/include")

if(NOT EXISTS "${LibVNCServer_BINARY_DIR}/include/rfb/rfbconfig.h")
	message(FATAL_ERROR
		"libvncserver did not generate ${LibVNCServer_BINARY_DIR}/include/rfb/rfbconfig.h. "
		"The build would pick up a different set of feature macros than the library was "
		"compiled with, which links and then misbehaves.")
endif()

add_library(LauncherVnc INTERFACE)
add_library(Launcher::Vnc ALIAS LauncherVnc)

target_link_libraries(LauncherVnc INTERFACE vncserver)
target_include_directories(LauncherVnc INTERFACE ${LAUNCHER_LIBVNCSERVER_INCLUDE_DIRS})
