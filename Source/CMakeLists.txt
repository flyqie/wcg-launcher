
add_subdirectory(Common)

add_executable(Launcher
	main.cpp

	"${CMAKE_BINARY_DIR}/Launcher.rc"

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

target_link_libraries(Launcher PRIVATE LauncherCommon Launcher::Vnc)

if(TARGET LauncherJpeg)
	add_dependencies(Launcher LauncherJpeg)
endif()

add_custom_command(TARGET Launcher POST_BUILD
	COMMAND "${CMAKE_COMMAND}"
		"-DEXE=$<TARGET_FILE:Launcher>"
		"-DLINKER=${CMAKE_LINKER}"
		-P "${CMAKE_SOURCE_DIR}/cmake/CheckDependencies.cmake"
	COMMENT "Checking that Launcher.exe needs nothing but Windows")
