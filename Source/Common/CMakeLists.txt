
add_library(LauncherCommon STATIC
	LauncherEnv.cpp
	LauncherLog.cpp)

launcher_configure_target(LauncherCommon)
