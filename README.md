# Launcher

A Windows container has no display adapter, so a GUI application running in one has nowhere
to draw. **Launcher gives it somewhere.**

It builds a window station and a desktop inside the container, starts the program you name
on that desktop, paints every window into a bitmap, and — if you ask for it — serves the
result over VNC.

![RealVNC Viewer showing a Server 2025 container: Notepad with an About box on the desktop,
and the dock along the bottom](screenshot.png)

Process-isolated containers only (`--isolation process`). Based on
[windows-container-view](https://github.com/smallmodel/windows-container-view) by
[smallmodel](https://github.com/smallmodel), which is where the window-station work came
from.

## Requirements

Windows 10 or later, or Windows Server, x64 · Visual Studio 2019 or later with the "Desktop
development with C++" workload · CMake 3.21 or later · Docker · a VNC viewer. Three git
submodules live under `deps/`, so a clone needs `git submodule update --init`.

## Building

```powershell
cmake --preset windows-x64
cmake --build --preset release
```

`Output\x64\Release\Launcher.exe` needs nothing but system DLLs, and the build checks that
rather than quietly producing something the container would need more than one file for.

## Running

Everything runs inside the container:

```bat
docker run --rm -ti --isolation process -p 5900:5900 ^
  -e LAUNCHER_VNC=1 -e LAUNCHER_DOCK=1 ^
  -v C:\path\to\Launcher.exe:C:\Launcher\Launcher.exe:ro ^
  mcr.microsoft.com/windows/server:ltsc2025 ^
  C:\Launcher\Launcher.exe notepad.exe
```

Then `vncviewer <host address>:0`. Anything after the program name is what starts on the
container's desktop; with nothing, `cmd.exe` starts. Paths with spaces are re-quoted.

The image must be **Server Core** or later — Nano Server removes Win32k and cannot create
windows — and its build must match the host's, or process isolation will refuse to start.

`LAUNCHER_DOCK=1` adds a strip of window buttons along the bottom: a container has no shell,
so a minimized window would otherwise be lost. Left-click restores, raises or minimizes;
middle-click closes.

## Configuration

Everything is an environment variable; unset means the default.

| Variable | Default | Meaning |
|---|---|---|
| `LAUNCHER_VNC` | none | `1` to serve the desktop over RFB. **Without it nothing listens** |
| `LAUNCHER_VNC_PORT` | `5900` | Port to listen on, 1024–65535 |
| `LAUNCHER_VNC_PW` | none | Password a viewer must give. Unset means **no authentication at all** |
| `LAUNCHER_RESOLUTION` | none | Desktop size, `WIDTHxHEIGHT`; unset leaves the container's stub driver default, 1024x768 |
| `LAUNCHER_DOCK` | none | `1` to draw the dock below the desktop |
| `LAUNCHER_FPS` | `30` | Capture rate, 1–240 |
| `LAUNCHER_DEBUG` | none | `1` to print anything at all |

VNC uses only the first eight characters of a password.

## How it works

`PrintWindow()` is the only way to get pixels out of a container — there is no DWM, so
nothing composites the screen and a window's device context holds nothing. It also only
works on a window station that allows I/O, so the program grants everyone access to
`WinSta0`, attaches to it, and starts the requested program there. From then on it paints
every window into one bitmap, marks what changed since the last frame, and lets libvncserver
encode and send that on its own threads. The cursor travels beside the frame rather than in
it, because RFB carries it separately.

## Known limitations

Themes draw imperfectly — with no DWM, `WM_PRINT` does not fully support themed controls, so
dialog buttons flicker on a fraction of frames, and every window keeps the resize border DWM
would hide. The cursor shape is unreliable. Nothing is captured while no viewer is connected.
About 30% of one core per container at full rate for the capture side, less when the screen
is still. No audio.

## Licence

**GPL-2.0-or-later.** Launcher links [LibVNCServer](https://github.com/LibVNC/libvncserver)
statically, so the whole of `Launcher.exe` is a derivative work under the same terms; the
full text is in [LICENSE](LICENSE), and it may be changed to GPL-2.0-only.
[THIRD-PARTY.md](THIRD-PARTY.md) lists everything that goes into the binary, and the release
workflow ships a source archive with it.
