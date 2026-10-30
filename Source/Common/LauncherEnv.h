#pragma once

bool LauncherVnc();

int LauncherVncPort();

const char* LauncherVncPassword();

bool LauncherResolution(int& Width, int& Height);

bool LauncherDock();

int LauncherFps();

bool LauncherFlag(const char* Name);
