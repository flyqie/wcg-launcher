# What the rest of the program shares: LauncherEnv (the environment variables that
# differ between deployments) and LauncherLog (the Debug() stream).

add_library(LauncherCommon STATIC
	LauncherEnv.cpp
	LauncherLog.cpp)

launcher_configure_target(LauncherCommon)
