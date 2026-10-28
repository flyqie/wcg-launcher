# Building and running

Build, deployment and troubleshooting for Launcher, plus the things that
were established the hard way while getting it to work.

Architecture and use cases are in the `README.md` at the repository root.

---

## 1. Requirements

| Component | Notes |
|---|---|
| Visual Studio 2019 or later | The "Desktop development with C++" workload. The projects are CMake, so any toolset the generator offers works |
| CMake 3.21 or later | See section 3 for which one to use — the newest CMake is not always the right one |
| Three submodules | See section 2. Nothing else to install, and nothing is looked for on the machine |
| Docker | To run the container |
| A VNC viewer | To look at the container. Any of them |

**x64 only.** A 32-bit configure is refused rather than built.

---

## 2. What it is built from

Everything comes from `deps/`, as git submodules pinned to a version each:

| Submodule | Version | What it is |
|---|---|---|
| `deps/libvncserver` | LibVNCServer 0.9.15 | The RFB server |
| `deps/zlib` | v1.3.2 | The ZRLE and Tight encodings |
| `deps/libjpeg-turbo` | 3.2.1 | Tight's JPEG subencoding |

Nothing is downloaded at configure time and nothing is searched for on the machine. A
clone has to fetch them:

```powershell
git clone --recurse-submodules <repository>
# or, in an existing clone:
git submodule update --init
```

**Not `--recursive`.** libvncserver has two submodules of its own — a web client and a
stylesheet for its documentation — and neither is built here.

The build stops with a message naming the missing one if any of the three is absent.

### libvncserver

`add_subdirectory` rather than a package, because the version is pinned by the submodule
and the RFB server is part of this program rather than a system library it happens to
use. That is what buys a static `vncserver.lib` with no DLL to deploy. Three things are
arranged around it in `cmake/LibVNCServer.cmake`, each worth knowing before changing it:

- **Its `CMakeLists.txt` asks for CMake 3.4, and CMake 4 refuses anything below 3.5.**
  `CMAKE_POLICY_VERSION_MINIMUM` lifts the floor; without it the configure stops on
  libvncserver's first line.
- **It exports no usage requirements.** Its include directories are global to its own
  directory, so the include paths — including the build tree's, where the generated
  `rfbconfig.h` lives — are carried by the `Launcher::Vnc` interface target instead.
  Getting the second one wrong is not a missing header but a silently different build,
  so it is checked rather than assumed.
- **It does not select an MSVC runtime library.** The default is `/MD`, which would make
  the container need a VC++ redistributable and defeat the point.

### What the two libraries buy

libvncserver compiles some of its encodings only when it finds a library to do the
compression with. Both are here, so all of these exist:

| Encoding | Needs | Notes |
|---|---|---|
| Raw | — | Always available; the fallback |
| CopyRect | — | For scrolling, which is a large part of what a desktop does |
| RRE, CoRRE | — | Cheap for flat areas |
| Hextile | — | The classic general-purpose one |
| Ultra | — | libvncserver's own miniLZO |
| Zlib, ZRLE, ZYWRLE | zlib | The general-purpose compression, and what most viewers ask for first |
| Tight | zlib **and** libjpeg | Its JPEG subencoding is the only one that helps photographic content |

Two that could exist do not, and neither is worth a submodule: libpng would add only
TightPng, and the real LZO rather than the miniLZO libvncserver carries changes nothing,
since Ultra exists either way. **TLS has no switch at all** — a switch would only buy a
build that quietly started carrying OpenSSL.

**`LAUNCHER_VNC_PURE`** (OFF) is the only switch in this project. On, neither library is
built and neither encoding exists, for a build only ever looked at over loopback:

```powershell
cmake -S . -B build -DLAUNCHER_VNC_PURE=ON
```

Then libvncserver uses what it carries itself. The profile says whether that matters:
`cpu us/frame … so N elsewhere` is what the encoder costs and `kB sent` is what it
produced. On loopback both are small enough that compressing further is CPU spent for
nobody; on a link that is not loopback it is the difference between usable and not,
which is why the default is to build them.

### How they are wired in

Neither goes through `find_package`, which would look for a header and a library on the
machine and take whatever it found. What libvncserver links has to be static, so the
search is turned off and the answers are supplied instead — which is also what keeps a
static build of each: there is nothing to find, and therefore nothing to find wrong.

**zlib is a subdirectory**, which it is happy to be. Two of its own options are set
first, both about not building what this program cannot use: it builds a shared library
by default, which would be a zlib DLL the container has to carry, and its examples. Its
static library is `zlibstatic`; `zlib` is the shared one, and linking that by mistake is
exactly what the check below is for.

**libjpeg-turbo is not a subdirectory**, because it refuses to be one, in its own words:

```
The <project> build system cannot be integrated into another build system
using add_subdirectory(). Use ExternalProject_Add() instead.
```

So it gets a CMake run of its own. The cost is a library to point at rather than a target
to link, and an ordering dependency to state by hand; the benefit is that neither build
system has to know anything about the other. It builds statically and selects the static
C runtime itself, so it needs no telling about either.

**Its SIMD is off**, deliberately. That is assembly, and assembling it needs NASM — a
fourth submodule, a build of its own, and a configure-time sub-build, because
libjpeg-turbo looks for the assembler by *running* it during the configure. What it
speeds up is the JPEG subencoding of Tight, the least-used path here.

### Keeping it one file

The container-side program is one file: everything it needs is either linked into it or
part of Windows. Submodules rather than system libraries is what makes that possible, and
it is also what makes it worth checking — a build that quietly picked up a DLL build of
zlib would be one the container could not run.

**The build checks, and fails if it is broken.** After linking, `Launcher.exe`'s import
table is read and anything that is not in `System32` is an error:

```
Launcher.exe depends on DLLs that are not part of Windows:
    zd.dll

The container-side program has to be one file, so this build cannot be deployed.
```

The check is `cmake/CheckDependencies.cmake`, run from `Source/CMakeLists.txt`. It caught
`zd.dll`, zlib's Debug shared library, the first time this was wired up.

The examples, the Qt client, the GTK and SDL example clients and the test suite are off
as well. None of them is used here.

---

## 3. Building

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Output lands in `Output\x64\<Configuration>\`:

| File | Needs at runtime |
|---|---|
| `Launcher.exe` | **Nothing but system DLLs** |

**That is the whole deployment.** The C runtime is linked statically, and so are
libvncserver, zlib and libjpeg-turbo. Copy the executable into the container and there is
nothing else to carry — no DLLs of theirs, no VC++ redistributable, no second program.

The first build takes a few minutes longer than the ones after it, because all three
submodules are built from source. Nothing is downloaded.

Both configurations build with the settings the Visual Studio projects used
(`/W3 /permissive- /sdl`, whole-program optimisation in Release, a PDB in both), which
are in `cmake/CompileOptions.cmake`.

### Which CMake

The build needs a CMake that knows the Visual Studio you have. That is usually the system
one, but not always:

| Situation | What to use |
|---|---|
| Ordinary install | `cmake` from PATH, 3.21 or later |
| The generator is not found | Use the CMake Visual Studio ships, versioned with it: `…\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |

> On the machine this was set up on, the system CMake was 4.5.0-rc1 and did not know the
> installed Visual Studio 2026 (generator `Visual Studio 18 2026`), while the one inside
> Visual Studio did. `cmake --help` lists the generators; if yours is not there, that is
> the reason, not the project.

`cmake -S . -B build` picks the newest Visual Studio it knows and, on an x64 host, x64.
Add `-A x64` if it picks something else. Ninja works too, from a developer command
prompt:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
```

### The build directory

`build/` is the default and is in `.gitignore`. Nothing is written outside `build/` and
`Output/`, which is inside the source tree on purpose: it is the layout this document and
the docker command below refer to. It can be moved with `-DLAUNCHER_OUTPUT_DIR=…`.

---

## 4. Running

### 4.1 The program

One executable, run **inside the container**. It builds a desktop there, starts the
program it was asked to start, and serves the result over RFB:

```
Launcher.exe [command line]
```

The command line is **the program to start on the container's desktop**; with none it is
`cmd.exe`. It is not the port and not an address — this program listens rather than dials.

**Serving is opt-in.** With `LAUNCHER_VNC=1` the program serves the desktop over RFB.
Without it, it still builds the desktop and starts the program on it and still ends when
that program does — it just serves nothing, and does not capture either, because the
capture is the whole cost of the loop. That is the mode to run a GUI program in a
container with nothing attached to it.

### 4.2 Firewall

**Nothing listens unless `LAUNCHER_VNC=1` is set.** With it, the program listens on
**5900**, changeable with `LAUNCHER_VNC_PORT`. Docker's NAT usually means nothing has to
be opened on the host, but a container on another machine, or a viewer outside the host,
needs the port reachable.

To confirm it is listening, from the host:

```powershell
Get-NetTCPConnection -State Listen -LocalPort 5900 | Select LocalAddress, LocalPort
```

Or from inside the container:

```
powershell -c "Test-NetConnection 127.0.0.1 -Port 5900"
```

### 4.3 The container

The image must be **Server Core** or later; Nano Server removes Win32k and cannot create
windows. **This project is for `--isolation process` only**; Hyper-V isolation is not
supported.

```
docker run --rm -ti --isolation process ^
  -p 5900:5900 ^
  -e LAUNCHER_VNC=1 ^
  -v C:\path\to\Launcher.exe:C:\Launcher\Launcher.exe:ro ^
  mcr.microsoft.com/windows/server:ltsc2025 ^
  C:\Launcher\Launcher.exe
```

The command-line argument is the program to start on the container's desktop:

```
  C:\Launcher\Launcher.exe notepad.exe
  C:\Launcher\Launcher.exe "C:\Program Files\App\app.exe"
```

Paths with spaces are re-quoted, so they can be passed directly. Then point a viewer at
the host:

```
vncviewer <host address>:0
```

With `-p 5900:5900` that is the host itself; a container on another machine needs that
machine's address and its firewall open.

> **The image build must match the host's**, or process isolation will refuse to start.

### 4.4 What to expect

A viewer connecting sees the container's desktop — the background colour plus whatever
program was started — and can drive it with the mouse and keyboard. With
`LAUNCHER_DOCK=1` there is a strip of window buttons below it; see section 4.5.

**It is named after the container**: `Windows Container - <computer name>`, because
a viewer with several of these open has nothing else to tell them apart by.

**The clipboard works in both directions.** Copy in the container and the viewers are
told; paste in a viewer and the container's clipboard is replaced. See section 4.6.

Several viewers can be connected at once and share the session. The pointer is shared
too, the way VNC normally works: whoever moves it moves it for everyone.

**The program prints nothing by default.** To see output, set `LAUNCHER_DEBUG=1`; it then
prints a per-second frame profile (section 7). Without it:

| What you see | Meaning |
|---|---|
| No output, process keeps running, port listening | Normal |
| No output, exit code 1 | Initialisation failed — the window station could not be reached, or the RFB port could not be bound. Set `LAUNCHER_DEBUG` to see which |
| Exits on its own, exit code 0 | The program it started has closed. The desktop has to stay empty for two seconds first, because a window exists before it is shown and one empty look at it is not proof that anything is gone |

The program does not outlive the program it launches: close that program's last window
and the session ends.

### 4.5 The dock

Set **`LAUNCHER_DOCK=1`** and a strip of window buttons appears along the bottom of the
desktop: one per top-level window in the container, with its title.

A container has no shell — no taskbar, no Alt+Tab — and a minimized window has no pixels
for the capture to paint, so without the dock, minimizing a window loses it.

| Click on | Result |
|---|---|
| A minimized window's button (drawn dimmed) | Restores it and brings it forward |
| A background window's button | Brings it forward |
| The button of the window already in front | Minimizes it |

**It behaves the way a real taskbar does, in both directions.**

*Drawn over the desktop.* The frame is exactly the screen, so the viewer's window is the
same size whether the dock is on or not, and dragging a window down slides it behind the
dock rather than under the pointer.

*Reserved out of the work area.* The program narrows the container's work area by the
dock's height, which is what a taskbar does with the space it occupies. Maximized windows
stop at the dock instead of disappearing behind it, and anything that centres itself in
the work area is centred in what is left. **The container sees a smaller usable area, not
a smaller screen** — `SM_CYSCREEN` and `GetDeviceCaps(VERTRES)` are unchanged.

The reservation is read back and checked, and reported under `LAUNCHER_DEBUG=1`:

```
Work area reserved: bottom 26 rows
```

If it says `Work area could not be reserved` or `Work area stayed 800x600 rather than
1024x768`, the system refused it and maximized windows will extend behind the dock.
Everything else about the dock still works.

**The viewer is not told the dock exists.** The frame is the screen, the desktop still
starts at its top-left corner, and the work area is the container's own business. What
needs care is the cursor: a dock click never reaches the container, so the container's
cursor is never moved there, and the cursor the viewer draws follows the viewer's own
pointer instead.

> The buttons keep the order the windows first appeared in, not z-order — z-order changes
> every time a window is activated, and buttons that reshuffle under the cursor cannot be
> clicked. And a minimized window is never drawn as the active one, even when it is still
> the foreground window, which it can be: minimizing a window does not necessarily hand
> the foreground to anything else.

### 4.6 The clipboard

Both directions, without a switch: RFB has carried cut text since the beginning, and a
remote desktop where paste does not work is a remote desktop people work around.

| Direction | How it is noticed |
|---|---|
| Container → viewers | The clipboard's change counter, checked once a frame. The clipboard itself is opened only on the frames where it moved |
| Viewers → container | libvncserver's cut-text callback, queued and applied by the frame loop like everything else that touches the container |

The counter is what makes the first direction affordable: `GetClipboardSequenceNumber`
costs nothing and needs no window and no message loop, which a frame loop has not got.

**What it carries is bytes, and the encoding is a guess.** RFB's cut text has no encoding
field: what goes out is UTF-8 bytes and what comes in is sniffed. A viewer that reads
UTF-8 when it is valid shows anything; a strict Latin-1 viewer shows mojibake for
anything outside it.

libvncserver does have the part of the protocol that says UTF-8 in so many words — the
extended clipboard — and the default build compiles it in, since it lives inside
`#ifdef LIBVNCSERVER_HAVE_LIBZ`. **This program does not use it**: it sends through
`rfbSendServerCutText`, the plain path, where the extended one is
`rfbSendServerCutTextUTF8` and only applies to viewers that negotiated it. So the guess
above is what happens in both builds, and `LAUNCHER_VNC_PURE` changes only the encodings.

Both halves of that guess are in `Desktop::SetClipboardText` and `ReadClipboardText`, and
the incoming one is the mirror of what viewers do: try UTF-8, fall back to Latin-1 when
the bytes are not valid UTF-8. A byte sequence that is valid UTF-8 and also meaningful as
Latin-1 — `c3 a9`, which is "é" either way — goes the UTF-8 way.

**The clipboard is shared, so anything on it is.** A password copied on the host is a
password the container's programs can paste, and the other way round. Worth knowing
before turning this on a machine whose clipboard carries more than it should.

---

## 5. Configuration

Everything is an **environment variable**. Unset means the default.

| Variable | Default | Meaning |
|---|---|---|
| `LAUNCHER_VNC` | none | Set to `1` to serve the desktop over RFB at all |
| `LAUNCHER_VNC_PORT` | `5900` | Port the RFB server listens on. 1024–65535 |
| `LAUNCHER_VNC_PW` | none | Password a viewer must give; unset means no authentication |
| `LAUNCHER_RESOLUTION` | none | Container desktop size, `WIDTHxHEIGHT` |
| `LAUNCHER_DOCK` | none | Set to `1` to draw the dock below the desktop |
| `LAUNCHER_FPS` | `30` | Capture rate, 1–240 |
| `LAUNCHER_DEBUG` | none | Set to `1` to print anything |

The default port is in `Source/Common/LauncherConfig.h`. There is no host address
variable: this program listens rather than dials.

### LAUNCHER_RESOLUTION

The container has no display adapter. win32k falls back to a stub display driver built
into itself, which advertises a fixed set of modes; the program asks for one of them
before starting the program, so it opens at the right size.

```
-e LAUNCHER_RESOLUTION=1920x1080
```

Only these are accepted, all at 32bpp and 60 Hz:

```
1024x768   640x480    800x600    1152x864   1280x768   1280x800
1280x1024  1440x900   1600x1200  1680x1050  1856x1392  1920x1200
2560x1600  1600x768   1600x900   1920x1080  1024x600   1366x768
```

Anything else is refused by the driver, and the default — 1024x768 — is used instead.
With `LAUNCHER_DEBUG=1` the program says which happened. See
[IMAGE-ANALYSIS.md](IMAGE-ANALYSIS.md), section 4.

### LAUNCHER_FPS

The capture cost is roughly fixed per frame, so this trades container CPU for smoothness:
30 fps is about 30% of one core, 15 fps about half that. See
[PERFORMANCE.md](PERFORMANCE.md).

**It is a period, and the profile prints the rate it produced.** A frame that overruns
its slot does not postpone the schedule: the next frame is due when it was always due,
and the rate falls to what the work allows rather than the loop running a burst to catch
up.

**It is also a ceiling rather than a promise.** While the screen is nearly still — under
1% of it changing per frame — the interval doubles, up to eight times, and goes straight
back to one the moment anything moves. The capture is three quarters of a frame and none
of it can be made cheaper, so not taking a frame is the only way left to spend less; the
profile showing 6 fps on a still screen is that working. Input wakes the loop directly,
so a click is applied and a frame taken at once whatever the back-off had reached.

**And it is the rate while somebody is watching.** With no viewer connected the capture
is skipped entirely and the loop sleeps, so an unwatched container costs almost nothing
rather than the ~30% of a core the figure above describes.

### The capture rate is not the frame rate

Worth knowing before tuning it:

* The capture rate is this program's, set by `LAUNCHER_FPS`, and it is what the container
  pays for.
* What a viewer receives is decided by what changed. A blinking caret costs a few hundred
  bytes; a window being dragged costs that window.

So lowering `LAUNCHER_FPS` does not lower the picture quality, only how quickly a change
is noticed.

---

## 6. Troubleshooting

### The viewer connects and the picture is blank or the wrong colour

The RFB server is up but the frame is not arriving. Check, in order:

1. Is a window actually on the desktop? A container with nothing visible in it is a blank
   desktop, and that is what it looks like.
2. Set `LAUNCHER_DEBUG=1` and read the frame profile (section 7). `PrintWindow calls per
   frame   max 0` means the capture found no windows at all.
3. If the picture is there but the colours are swapped — red and blue exchanged — the
   pixel format libvncserver was told about does not match the DIB. That is
   `Frame::ApplyServerFormat`, and it is not optional: a GDI DIB section is BGRA and
   libvncserver assumes RGBA from the byte count alone.

### Nothing listens on 5900

The program binds the port on its **first captured frame**, not at startup, because that
is when the screen size is known. A container whose desktop never produces a frame never
listens.

The bind failure is reported under `LAUNCHER_DEBUG=1`:

```
RFB server could not listen on port 5900
```

The usual cause is the port already being in use inside the container, or being below
1024, which the container's account cannot bind.

Check `LAUNCHER_VNC` first, though: without it the program never creates a server at all,
so the log says nothing about a port because it never asked for one.

### Typing does nothing, or types the wrong characters

RFB carries **characters, not keys**: a viewer sends the keysym for what was typed, so
Shift+A arrives as the keysym for "A" and a viewer with a different keyboard layout may
never send Shift at all. `Input::ApplyKey` resolves a keysym to a virtual key through
`VkKeyScanW` — which is what makes it follow the container's layout rather than a table —
and presses the modifier itself when the viewer is not holding it.

If a key does nothing at all, its keysym is not in `gKeySyms`, the table of named keysyms
(arrows, function keys, modifiers, keypad), and the container's layout has no key that
produces it. If letters come out as the wrong case, the modifier tracking is at fault:
`gHeld` is what the viewer is holding and `gTyped` is what was pressed here.

### The picture is there and the mouse moves, but clicking or typing does nothing

`SendInput for mouse returned false with error 5` in the log is the whole diagnosis: **5
is `ERROR_ACCESS_DENIED`, and it means the calling thread is not on the container's
desktop.** Input injection is per-desktop, so a thread anywhere else has everything
refused — `SendInput` fails outright, and `SetCursorPos` quietly moves a cursor on a
desktop nobody is looking at.

The desktop this program builds is attached with `SetThreadDesktop`, which is
**per-thread**, and a thread created afterwards does **not** inherit it: measured, a new
thread gets the desktop the process started on, not the one its creator was moved to. So
anything that injects input has to run on the thread that attached. That is why the
libvncserver callbacks post what the viewer asked for instead of injecting it, and
`Handover::Apply` on the frame loop is the only place that touches the desktop's input.

**If input breaks again, that invariant is what to check first.** Anything that calls
`Desktop::PointerMove`, `PointerButton`, `Wheel` or `Key` from a thread libvncserver owns
fails exactly this way, and the failure is silent for everything except the buttons.

Two smaller things, once the above is right:

- Positioning is done by `SetCursorPos`, and the `dx`/`dy` in `SendInput` are ignored
  because `MOUSEEVENTF_ABSOLUTE` is not set. That is intended.
- A click over the dock is handled by the dock and never reaches the container. See
  section 4.5.

### The cursor is wrong

A viewer hides its own cursor the moment the server tells it there is one to draw. So
whatever the server has as its cursor is what the user gets — including libvncserver's
own built-in placeholder, which is an **X**. That is what a viewer shows until this
program publishes something of its own, so an X on connecting means nothing had been
published yet.

Four things had to be right, and each was wrong at some point:

- **The order in the frame loop is load-bearing.** `Desktop::ReadCursor` reads the bitmap
  **only when the cursor handle changes**, so on a fresh session the first read is the
  only one that will ever produce one — and the RFB server has to exist before it, or the
  shape is read and thrown away.
- **Nothing is published until a shape has actually been seen.** `Cursor::Publish` returns
  without sending anything while it has no bitmap, rather than sending the empty cursor.
  Sending "draw nothing" is a different statement, and it leaves the user with no pointer
  at all rather than with the viewer's own.
- **The cursor-handle cache must not skip the fallback.** A container's desktop has no
  cursor handle of its own for the first seconds of a session, and the cache was comparing
  that NULL against its own initial NULL and returning before reaching the system-arrow
  fallback — so nothing was read, nothing was published, and the viewer sat on the X.
- **The container can only report its cursor intermittently, and neither call fixes
  that.** This one is a property of the container rather than a mistake here.
  `GetCursorInfo` reports the cursor the *session* is displaying, and a container has no
  session, so it returns `hCursor == NULL` on most frames — measured, 57 of 57 — while
  returning a correct position, because the position comes from somewhere else.
  `GetCursor` reads the calling thread's own queue instead, which is maintained
  unconditionally, but it holds what *this* thread last set and this program never calls
  `SetCursor`: measured, it returned the same 32x32 wait cursor on every frame of a
  session, which is worse than nothing. Both were tried; the disassembly and the
  measurements are in [IMAGE-ANALYSIS.md](IMAGE-ANALYSIS.md).

  What `Desktop::ReadCursor` does instead is take `GetCursorInfo`'s handle when it is
  there and show the standard arrow when it is not, with the arrow loaded once and the
  bitmap cached against the handle — so a frame the desktop reports nothing for costs one
  comparison. **A cursor that will not change is the expected outcome in a container, not
  a fault to chase.**

`LAUNCHER_DEBUG=1` prints a line whenever a cursor is published, which is the quickest
way to tell the publishing faults apart:

```
Cursor published: 32x32 hotspot 0,0 opaque pixels 144; container cursor 512,384, viewer pointer 512,384
```

- **`opaque pixels` is what the mask actually draws.** Zero means the shape is there and
  empty — an all-transparent bitmap from the container, not a message that went missing.
- **No line at all** means nothing has been published, which is correct until the
  container shows a cursor and a fault only if it never does.
- **The two positions** answer the other question, for a cursor that arrives but never
  changes shape. If the container's cursor stays put while the viewer's pointer moves, the
  container is not being told where to point and nothing under it ever asks for a
  different cursor. If both move together, the shape is changing and something downstream
  is losing it.

And once per profile window, a count of what the cursor read actually found:

```
  cursor reads 56, no handle 56, bitmap read 0
```

**`no handle` equal to `reads` across a whole session is normal here** — it means the
container is not reporting a cursor, which is what the stand-in arrow is for. A large
`bitmap read` with a cursor that still will not change means the handle is churning
instead.

### The mouse wheel does nothing

Three things have to line up, and the first is the likely one:

1. **The pointer has to be over the container's desktop.** A scroll over the dock is
   dropped: nothing down there scrolls.
2. **Only whole notches cross.** A precision touchpad reports fractions, which are
   accumulated by the viewer until they make up one notch, so a very slow two-finger drag
   takes a moment to register. A mouse's wheel is unaffected.
3. **The RFB wheel bits have to be understood by the viewer.** They are buttons 4 to 7 in
   the pointer mask, which every viewer sends.

If it is still dead, set `LAUNCHER_DEBUG=1`: `SendInput for wheel returned false` means
the injection itself was refused, which is a different problem from the event never
arriving.

### A window I minimized has vanished

A minimized window has no pixels, so the capture skips it, and there is no shell in the
container to restore it from. With `LAUNCHER_DOCK=1` its button is in the dock, drawn
dimmed; click it to bring the window back.

If a window has no button at all, it is either not visible, has no title, or is a tool
window (`WS_EX_TOOLWINDOW`), which the dock filters out the way a taskbar does.

### A maximized window has a strip of bare desktop down its right edge

Windows gives a maximized window an **invisible resize border**, so `GetWindowRect` on one
returns a rect *larger than the display* — `(-8, -8, 1032, 750)` on a 1024x768 screen.

`PrintWindow` paints a window into the device context by its rect, so the part that is
actually on screen starts at the border offset rather than at the context's corner. A
scratch bitmap the size of the screen therefore clips the far edge away **before the copy
out**, and no amount of adjusting the `BitBlt` afterwards can recover it — the columns
were never painted.

`DesktopCapture.cpp` allocates the scratch at the screen size plus twice
`SM_CXSIZEFRAME + SM_CXPADDEDBORDER`, clamps the destination to the screen, and offsets
the source by the same amount. The descendants are still positioned against the
**unclamped** rect origin, because their coordinates are relative to it.

> The bottom edge is affected identically and is easy to miss: without the dock the work
> area is the whole screen, so a maximized rect's bottom runs past it and the last rows
> are lost too. The dock covers them.

### Every window has a thick border around it

The border is real, and it cannot be removed.

Windows gives every resizable window an invisible resize border,
`SM_CXSIZEFRAME + SM_CXPADDEDBORDER` — 8 px at 100% DPI. On a normal desktop that is the
grab area, and DWM draws a hairline frame on top of it, so it is never seen. **There is no
DWM in the container**, and `PrintWindow`'s `PRF_NONCLIENT` pass goes through the classic,
non-composited non-client painting, which fills all 8 px in as a visible frame.

The alternatives are worse. `PW_CLIENTONLY` skips the non-client area entirely, which
takes the title bar with it — nothing left to drag a window by, and no close button.
Drawing the frame and title bar by hand means reimplementing themed controls, which this
container already cannot paint reliably.

### The status bar flickers, or dialog buttons do

There is **no DWM in the container** (`tasklist /fi "imagename eq dwm.exe"` returns
nothing), so there is no compositor and no screen surface to read. `BitBlt` from a window
DC produces nothing usable, and **`WM_PRINT` is the only way to get pixels** — which is
why the whole capture is built on it and on nothing else.

`PrintWindow`'s `PRF_CHILDREN` pass is unreliable for themed common controls:

| Control | Symptom | Status |
|---|---|---|
| Status bar `msctls_statusbar32` | Background only; parts and text missing | Fixed: `PrintWindowTree()` re-prints every descendant explicitly |
| Dialog buttons `Button` | Intermittently blank or missing their text, on about 12% of frames | **Unresolved**; a limitation of `WM_PRINT` with no DWM |

To tell whether a given control is affected, capture a run of frames and diff consecutive
ones: the changed region lands exactly on that control's rectangle.

```
frame018 -> frame019 : 811 px  bbox=(591,572)-(690,599)   <- the OK button
```

Directions that were tried and **did not work**, so they need not be retried:

| Attempt | Result |
|---|---|
| Clearing the scratch bitmap | No effect (but it removed stale-pixel artefacts, so it stayed) |
| Capping capture at 20 fps | No effect on flicker (but it cut CPU a lot, so it stayed) |
| `BitBlt` from the window DC | Picture completely unusable |
| `PW_RENDERFULLCONTENT` | Unusable without DWM |
| `RedrawWindow(RD_UPDATENOW)` then `PrintWindow` | Made every child `PrintWindow` fail |
| Printing each child twice | No effect |
| Re-printing only status-bar classes | Buttons still flickered |

Also note that **`PrintWindow`'s return value cannot be trusted**: `PrintWindow failed for
child Edit` appears every frame while the edit control renders perfectly.

---

## 7. Debug output and switches

### LAUNCHER_DEBUG

**The program prints nothing by default.** With `LAUNCHER_DEBUG=1` set, it prints a
per-second frame profile and its startup progress.

```
-e LAUNCHER_DEBUG=1        # to docker run
```

Everything goes through `Debug()` in `Source/Common/LauncherLog.h`, which discards when
the variable is unset. Failures are **not** reported by printing: a failure the caller can
act on travels as a return value and an exit code (section 4.4).

### The frame profile

Each line is one phase of the frame, in microseconds, over the last window of samples, and
each carries `mean`, `p50`, `p95` and `max`:

| Line | What it times |
|---|---|
| `capture` | `Desktop::Capture` and `Desktop::Finish` together |
| `dock` | `Dock::Draw` |
| `cursor` | `Desktop::ReadCursor`, and publishing a shape when it changed |
| `input` | Draining what the viewers asked for and injecting it into the desktop |
| `diff` | Comparing against the framebuffer last served, and copying what changed |
| `PrintWindow calls per frame` | The peak, which is what `capture` scales with |
| `capture split` | `alloc`, `fills` and `windows` |
| `of which windows` | `toplevel` and `child`, each with a per-call cost |
| `mean frame` / `worst frame` | The whole loop as a share of the `LAUNCHER_FPS` budget, and its inverse in containers per core |

Three more lines before those, which are counts rather than timings:

| Line | What it counts |
|---|---|
| `skipped` | Frames not taken because no viewer was connected. Printed only when there were any |
| `marked` | Rectangles marked per frame, and the most in any one frame — the area the encoder and the link pay for |
| `cpu us/frame` | This thread's CPU, the whole process's, and the difference — which is libvncserver's encoder and sockets, the part this program cannot time |

The header line counts the frames, the rate they came out at, how many of them had
anything to send, how many viewers were connected and how many bytes libvncserver sent:

```
--- frame profile: 60 frames in 2016 ms (29 fps), 7 with changes, 1 viewer(s), 51 kB sent ---
```

**The rate is in there because `LAUNCHER_FPS` is the knob for both CPU and bandwidth**,
and a knob is worth nothing if the thing it turns cannot be read off. It should sit at the
configured rate; if it does not, the loop is being held up by something other than its own
pacing.

**`cpu us/frame` is the one that closes the loop on capacity.** Everything else measures
this program's own thread, and libvncserver's encoding runs on threads of its own, so
"containers per core" derived from `mean frame` is a floor and not the answer. The
process's CPU time counts every thread, so the gap between the two figures is what the
encoder and the sockets actually cost — the number to plan against.

**A worked example with real numbers is in [PERFORMANCE.md](PERFORMANCE.md) section 2**,
and it is worth reading the caveat there before trusting it: those were measured with the
PNG-and-RPC transport, so the `capture` and `dock` figures are current and the `encode`
and `send` ones are not.

> **Every frame is captured, while anyone is watching; there is no change detection and
> no capture switches.** Both were built, measured and removed. Skipping when the desktop
> appeared unchanged needed a change detector, and neither `SetWinEventHook` nor
> `GetUpdateRect` received anything at all in this container, so the skip degenerated to
> the one-second resync and the picture updated once a second.
>
> The one skip that *is* there is not a heuristic: with no viewer connected there is
> nowhere to send a frame, so the frame is not taken. What did change with the transport
> is that *sending* is selective even though capture is not: the comparison decides which
> region is marked modified, so a frame that captures the whole screen still costs the
> network almost nothing when only a caret moved.
>
> The cost of capturing every frame is known and fixed: **30 fps of capture is about 30%
> of one core**, with no heuristics involved.

### Measuring

Costs vary enough between window contents that measuring only one case gives the wrong
answer:

| Scene | `PrintWindow calls per frame` | Notes |
|---|---|---|
| Empty desktop | 1 | The floor |
| `cmd.exe` console | 1 | The cost of one full-screen window |
| **A dialog with a dozen children** | **~15** | **The ceiling**; open a property sheet with `rundll32` |

---

## 8. Known limitations

- **No authentication unless `LAUNCHER_VNC_PW` is set.** With it unset — the default —
  anything that can reach the port can watch the container and drive it, so a trusted
  network only. The password is checked by libvncserver's `rfbCheckPasswordByList`, which
  means **the first eight characters of it and nothing after that**: VNC derives a DES key
  from the password and DES keys are 56 bits. A longer one is silently truncated by both
  ends rather than rejected.
- **The cursor shape is unreliable, and that is the container's doing.** See section 6,
  "The cursor is wrong", and [IMAGE-ANALYSIS.md](IMAGE-ANALYSIS.md).
- **Dialog buttons flicker.** With no DWM, `WM_PRINT` does not fully support themed
  controls. Turning the descendant re-print off saves about 1.1 ms and brings the flicker
  back, so it is a trade rather than a saving.
- **Viewers share one pointer.** Several can be connected, but they share a keyboard focus
  and a pointer, the way VNC normally works.
- **Input is applied when it arrives, not when the next frame does.** A click lands as it
  is made rather than up to a `LAUNCHER_FPS` interval later, because the queue wakes the
  loop. What it *produces* still appears at the next frame, because that is when the
  screen is captured. The cursor the viewer draws follows the viewer's own mouse directly.
- **Nothing is captured while no viewer is connected.** The loop skips the frame entirely
  and sleeps, so an unwatched container costs almost nothing. A viewer connecting is
  noticed on the next slot, and the first frame it gets is a full one.
- **No frame rate ceiling on the wire.** The capture is paced by `LAUNCHER_FPS`, but what
  is sent is whatever changed. A screen changing continuously — a video playing — is sent
  as fast as libvncserver can encode it.
- **Capacity: about 3 containers per core on an idle desktop, about 1 with a dialog
  open** — for the capture side, which is what the profile measures. The encoder runs on
  libvncserver's threads and is not in that number, so the real figure is lower. Numbers
  and experiments in [PERFORMANCE.md](PERFORMANCE.md).
- **No audio.** Containers have no audio endpoint and no stream to capture; see
  [AUDIO.md](AUDIO.md).
- **`IMAGE-ANALYSIS.md` was written against `servercore:20H2`**, while the project now
  uses `windows/server:ltsc2025`. The two differ substantially. That document opens with
  a correction, and its frame-path measurements are from the RPC design this replaced.
