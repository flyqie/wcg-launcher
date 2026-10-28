#pragma once

// The remote display: the RFB server that carries the desktop to a viewer and the
// viewer's input back. libvncserver does the encoding, the framing and the sockets;
// what is here is the server's lifetime, the frame loop, and the diff that decides
// which regions are worth sending.
//
// Runs until the container's windows are gone, and returns false if the server could
// not be brought up. Serving at all is opt-in through LAUNCHER_VNC; the port comes from
// LAUNCHER_VNC_PORT and the password from LAUNCHER_VNC_PW. See Common/LauncherEnv.h.
namespace Server
{
	bool Run();
}
