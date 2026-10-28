# Launcher

A Windows container has no display adapter, so a GUI application running in one has
nowhere to draw. **Launcher gives it somewhere.**

It builds a window station and a desktop inside the container, starts the program you
name on that desktop, paints every window into a bitmap, and — if you ask for it —
serves the result over VNC so any viewer can see and drive it.

![RealVNC Viewer showing a Server 2025 container: Notepad with an About box on the
desktop, and the dock along the bottom](Docs/screenshot.png)

*Notepad and an About box on a `windows/server:ltsc2025` container, seen through
RealVNC Viewer. The strip of window buttons along the bottom is the dock.*

It is a launcher first. The program it starts is the only thing worth varying per
deployment; everything else is optional.

| | |
|---|---|
| **Launching** | Always. The desktop, the program on it, and a session that ends when that program does |
| **The dock** | `LAUNCHER_DOCK=1` — a strip of window buttons, so a minimized window can be brought back |
| **Remote access** | `LAUNCHER_VNC=1` — RFB on `LAUNCHER_VNC_PORT`, for any VNC viewer |

It is for **process-isolated containers only** (`--isolation process`).

## Credits

**This project is based on
[windows-container-view](https://github.com/smallmodel/windows-container-view) by
[smallmodel](https://github.com/smallmodel).** The idea is theirs, and so is the hard
part: the window-station work that makes a container's desktop usable at all, and
`PrintWindow` as the only way to get pixels out of one.

Without that project this would not exist. Thank you.

What is different here is the shape and the transport. The original is a client that
dials a host-side program and pushes frames at it; this is a launcher that starts a
program inside the container and serves the desktop itself, over RFB, with
[libvncserver](https://github.com/LibVNC/libvncserver) doing the encoding and the
sockets. The measurements the rework rests on — what a frame costs, what was tried and
did not work — are in [Docs/PERFORMANCE.md](Docs/PERFORMANCE.md) and
[Docs/IMAGE-ANALYSIS.md](Docs/IMAGE-ANALYSIS.md).

## Requirements

* Windows 10 or later, or Windows Server, x64
* Visual Studio 2019 or later, with the Desktop development with C++ workload
* CMake 3.21 or later
* Docker, to run the container
* A VNC viewer, if you want to look at it

Building produces one program, `Launcher.exe`, and it needs nothing but system DLLs —
**and the build checks that**, failing rather than quietly producing something the
container would need more than one file for.

The RFB server and the two compression libraries behind the ZRLE and Tight encodings
are built in from submodules under `deps/`, so a clone needs
`git submodule update --init` before it will configure. Nothing else is installed and
nothing is looked for on the machine.

## Building

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The result lands in `Output\x64\Release\`. See [Docs/BUILD.md](Docs/BUILD.md) for which
CMake to use if the generator is not found, and for what the one option does.

## Running

Everything runs inside the container. Mount the executable in, name the program to
launch, and turn on whatever you want on top of it:

```bat
docker run --rm -ti --isolation process ^
  -p 5900:5900 ^
  -e LAUNCHER_VNC=1 ^
  -e LAUNCHER_DOCK=1 ^
  -v C:\path\to\Launcher.exe:C:\Launcher\Launcher.exe:ro ^
  mcr.microsoft.com/windows/server:ltsc2025 ^
  C:\Launcher\Launcher.exe notepad.exe
```

Then point a viewer at the host:

```
vncviewer <host address>:0
```

There is no host-side program to run — the container serves its own desktop, and the
viewer is whatever VNC client you already have. The session is named after the
container, and the clipboard works in both directions.

The image must be **Server Core** or later; Nano Server removes Win32k and cannot
create windows. Its build must match the host's, or process isolation will refuse to
start.

### Choosing what to run

Anything after the program name is started on the container's desktop. With nothing,
`cmd.exe` is started:

```bat
C:\Launcher\Launcher.exe notepad.exe
C:\Launcher\Launcher.exe "C:\Program Files\App\app.exe"
```

The command line is **the program to launch**, not an address or a port — this program
listens rather than dials. Paths with spaces are re-quoted, so they can be passed
directly.

With no viewer connected and nothing to look at, `Launcher.exe` still does its job:
it builds the desktop, starts the program, and ends when that program does. That is
the mode to run a GUI program in a container with nothing attached to it.

### The dock

Set `LAUNCHER_DOCK=1` and a strip of window buttons appears along the bottom of the
desktop, one per top-level window, with its title.

A container has no shell — no taskbar, no Alt+Tab — and a minimized window has no
pixels for the capture to paint, so without the dock, minimizing a window loses it.

| Click on | Result |
|---|---|
| A minimized window's button (drawn dimmed) | Restores it and brings it forward |
| A background window's button | Brings it forward |
| The button of the window already in front | Minimizes it |

It behaves the way a real taskbar does in both directions: it is drawn over the bottom
of the desktop, so the frame is exactly the screen and the viewer's window is the same
size either way; and it reserves the same rows out of the container's work area, so
maximized windows stop above it. The container sees a smaller usable area, not a
smaller screen.

### Remote access

Set `LAUNCHER_VNC=1` and the desktop is served over RFB. Without it nothing listens
and nothing is captured, because a frame nothing will be sent is not worth taking.

Several viewers can be connected at once and share the session, the way VNC normally
works — including the pointer.

## Configuration

Everything is an **environment variable**. Unset means the default.

| Variable | Default | Meaning |
|---|---|---|
| `LAUNCHER_VNC` | none | Set to `1` to serve the desktop over RFB. **Without it nothing listens** |
| `LAUNCHER_VNC_PORT` | `5900` | Port the RFB server listens on. 1024–65535 |
| `LAUNCHER_VNC_PW` | none | Password a viewer has to give. Unset means **no authentication at all** |
| `LAUNCHER_RESOLUTION` | `1024x768` | Container desktop size, `WIDTHxHEIGHT`. Only the modes the container's stub display driver advertises are accepted |
| `LAUNCHER_DOCK` | none | Set to `1` to draw the dock below the desktop |
| `LAUNCHER_FPS` | `30` | Capture rate, 1–240. The per-frame cost is roughly fixed, so this trades container CPU for smoothness — it is not the rate the viewer sees |
| `LAUNCHER_DEBUG` | none | Set to `1` to print anything at all |

VNC uses only the first eight characters of a password, so a longer one is truncated by
both ends. See [Docs/BUILD.md](Docs/BUILD.md) for the accepted resolutions and what
`LAUNCHER_FPS` does.

## How it works

`PrintWindow()` writes a window's appearance into a device context, but only for
windows on a window station that allows I/O — `WinSta0`. Anywhere else the result is
black, and container processes start on the service window station
(`Service-0x0-xxx$`), where the rights are also too low to create windows at all.

So the program, before doing anything else, grants everyone access to `WinSta0` and
`WinSta0\Default`, attaches itself to them, and starts the requested program on that
desktop. From then on it loops: paint every window into one bitmap, compare it against
what was last served, and mark the region that changed. libvncserver, on its own
threads, encodes and sends whatever is marked and hands back the input it receives.

The cursor is not painted into that bitmap. RFB carries it separately and the viewer
composites it, because a cursor baked into the frame dirties the pixels underneath it
every time the mouse moves.

### Source layout

Three parts. `Desktop` is the container's graphical environment, `Dock` is the strip of
window buttons along its bottom, and `Remote` carries the first of those to a viewer.
Neither of the other two knows that RFB exists, or that anyone is watching.

`Remote` is a directory rather than a file, because it is five things rather than one
step: `Server` (the server's lifetime and the frame loop), `Frame` (the framebuffer and
the diff), `Input`, `Cursor`, `Clipboard`, and `Handover` — the crossing from
libvncserver's threads to the frame loop's, and the one that makes the rest possible.
libvncserver delivers input on threads of its own, and those are not on the container's
desktop, so a callback that injected it directly would have everything refused; it
posts instead and the frame loop applies. [Docs/BUILD.md](Docs/BUILD.md) has the
measurement.

## Known limitations

* **No authentication unless `LAUNCHER_VNC_PW` is set.** With it unset — the default —
  anything that can reach the port can watch the container and drive it. Trusted
  networks only.
* **The cursor shape is unreliable.** A container's win32k does not maintain the record
  `GetCursorInfo` reports, so the cursor a viewer is shown is sometimes the container's
  and sometimes a plain arrow, and it can stay on one shape. See
  [Docs/IMAGE-ANALYSIS.md](Docs/IMAGE-ANALYSIS.md).
* **The clipboard is shared, so anything on it is.** Copy on the host and the container
  can paste it, and the other way round. Text outside Latin-1 needs a viewer that reads
  UTF-8, which is most of them: RFB's cut text carries no encoding of its own, and this
  program sends through the plain path either way.
* **Input is applied as it arrives.** Clicks, the wheel and keystrokes reach the
  container when you make them; what they *produce* appears at the next frame, because
  that is when the screen is captured. The cursor you see is not affected — it follows
  your own mouse directly.
* **Nothing is captured while no viewer is connected.** An unwatched container costs
  almost nothing; the loop skips the frame and sleeps until someone connects.
* **Themes draw imperfectly.** With no DWM in the container, `WM_PRINT` does not fully
  support themed controls. Status bars are handled by re-printing each descendant;
  dialog buttons still flicker on a fraction of frames.
* **Every window keeps its resize border.** DWM normally hides it. Without DWM,
  `WM_PRINT` paints all 8 pixels of it as a visible frame, and it cannot be removed
  without removing the title bars with it. It is visible in the screenshot above.
* **Capacity.** About 30% of one core per container on an idle desktop for the capture
  side; the encoder runs on top of that.
* **No audio.** Containers have no audio endpoint and no stream to capture; see
  [Docs/AUDIO.md](Docs/AUDIO.md).

## Documentation

| | |
|---|---|
| [Docs/BUILD.md](Docs/BUILD.md) | Building, deployment, every switch, and troubleshooting by symptom |
| [Docs/PERFORMANCE.md](Docs/PERFORMANCE.md) | Where a frame's time goes, and the experiments behind the current design |
| [Docs/IMAGE-ANALYSIS.md](Docs/IMAGE-ANALYSIS.md) | What is actually inside the container image, from disassembly |
| [Docs/RDP-IN-CONTAINER.md](Docs/RDP-IN-CONTAINER.md) | Why the RDP stack that ships in the image cannot be used |
| [Docs/AUDIO.md](Docs/AUDIO.md) | Why there is no audio, and what it would take |
