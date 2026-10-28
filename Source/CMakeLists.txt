# What the program is built from.
#
# Three parts. Desktop (the container's graphical environment) and Dock (the strip of
# window buttons along its bottom) are one file each; Remote (the RFB server) is a
# directory, because it is five things rather than one and each has a thread or a lifetime
# of its own. The boundaries are described in their headers.
#
# Common/ is what more than one part uses: the environment variables and the Debug()
# stream.

add_subdirectory(Common)

add_executable(Launcher
	main.cpp

	Desktop.cpp
	DesktopCapture.cpp
	Dock.cpp
	FrameProfile.cpp

	Remote/Server.cpp
	Remote/Frame.cpp
	Remote/Input.cpp
	Remote/Clipboard.cpp
	Remote/Cursor.cpp
	Remote/Handover.cpp)

launcher_configure_target(Launcher)
launcher_set_output_dir(Launcher)

# Nothing but system DLLs beyond what is listed here. Launcher::Vnc brings libvncserver
# and, through it, ws2_32; the rest is in CMake's default link libraries.
target_link_libraries(Launcher PRIVATE LauncherCommon Launcher::Vnc)

# libjpeg-turbo is built by an ExternalProject, which is a build step of its own rather
# than a target in this graph, so the ordering has to be said.
if(TARGET LauncherJpeg)
	add_dependencies(Launcher LauncherJpeg)
endif()

# And then check it, because "one file" is a property of the link that is lost silently.
# See cmake/CheckDependencies.cmake.
add_custom_command(TARGET Launcher POST_BUILD
	COMMAND "${CMAKE_COMMAND}"
		"-DEXE=$<TARGET_FILE:Launcher>"
		"-DLINKER=${CMAKE_LINKER}"
		-P "${CMAKE_SOURCE_DIR}/cmake/CheckDependencies.cmake"
	COMMENT "Checking that Launcher.exe needs nothing but Windows")
