# The container image: what is actually in it

Static analysis (IDA plus string scanning) of the container image, recorded so it
does not have to be dug out again.

**Two images are covered, and they differ substantially.**

| Image | Build | Analysed in |
|---|---|---|
| `mcr.microsoft.com/windows/servercore:20H2` | 10.0.19041.1889 | sections 1–4, 8 |
| `mcr.microsoft.com/windows/server:ltsc2025` | 10.0.26100.1150 | sections 5–8 |

Server Core's limits are **not** Windows container limits. The project now runs on
`windows/server:ltsc2025`, which is the full Server image, not Server Core, and
several conclusions drawn from 20H2 reverse on it. Where that happens it is called
out.

---

## 1. Graphics stack: GDI, and nothing else

The most valuable findings here are **negative** — they close off several
alternative ways of getting pixels.

Measured on `servercore:20H2`:

| Component | State | Consequence |
|---|---|---|
| `dwm.exe`, `dwmcore.dll`, `dwmredir.dll`, `dwmghost.dll` | **absent** | No compositor |
| `d3d10warp.dll` | **absent** | No WARP software rasteriser |
| `dcomp.dll` | **absent** | No DirectComposition |
| `magnification.dll` | **absent** | No `MagSetImageScalingCallback` |
| `mf.dll`, `mfreadwrite.dll`, `msmpeg2vdec.dll` | **absent** | No Media Foundation, no H.264 codec of any kind |
| `GdiPlus.dll` | present | **No longer used.** The client encoded frames as PNG through it; it now serves pixels over RFB and needs nothing but `gdi32` |
| `WindowsCodecs.dll` | present | WIC is available |
| `DWrite.dll`, `d2d1.dll` | present | Direct2D is there, but also needs an adapter |
| `d3d11.dll`, `d3d12.dll`, `dxgi.dll` | present | **And useless; see below** |

### Why `d3d11.dll` being present means nothing

`d3d11.dll` is only the user-mode runtime. `D3D11CreateDevice` needs either

1. a WDDM adapter — the container has no GPU and no display device, or
2. `D3D_DRIVER_TYPE_WARP`, the software adapter — which lives in
   `d3d10warp.dll`, **a file that is not in the image**.

Both routes are closed, so no D3D device can be created in the container. Capture
or rendering through D3D is a dead end.

### Conclusion

**`WM_PRINT` / `PrintWindow` is the only way to get window pixels in the
container**, with no alternative. It is not that other approaches were
overlooked; the capture is built on `WM_PRINT` and on nothing else.

### On `windows/server:ltsc2025` this table mostly reverses

The same scan on `windows/server:ltsc2025` (base plus cumulative update) finds:

| Component | 20H2 | **ltsc2025** |
|---|---|---|
| `dwm.exe`, `dwmcore.dll`, `dwmredir.dll`, `dwmghost.dll` | absent | **all present** |
| `dcomp.dll` | absent | **present** |
| `magnification.dll` | absent | **present** |
| `mf.dll`, `mfreadwrite.dll` | absent | **present** |
| `msmpeg2vdec.dll` (H.264 decode) | absent | **present** |
| An H.264 **encoder** MFT | none | **still none**: `msmpeg2enc.dll` is MPEG-2 only, and the encoder CLSID `{6CA50344-…}` is absent from the SOFTWARE hive and from every DLL under `System32` |
| `d3d10warp.dll` | **absent** | **present** |

Three consequences:

1. **The `WM_PRINT` conclusion still holds**, because DWM's binaries being present
   does not mean DWM runs. **`dwm.exe` launches and exits immediately**: it imports
   `D3DKMTOpenAdapterFromLuid` (open a WDDM adapter by LUID) and
   `RegisterSessionPort` (an interactive session), and a container has neither —
   its display is win32k's built-in `StubDisp` stub, not a WDDM miniport.
2. **The "no D3D device" conclusion needs re-testing.** WARP is now present, so
   `D3D11CreateDevice(D3D_DRIVER_TYPE_WARP)` is worth trying. WARP is pure CPU and
   offers no encoding acceleration.
3. **"No H.264 codec at all" becomes "Media Foundation and decoding are present,
   encoding is not".** Encoding still needs a bundled openh264, x264 or libavcodec,
   but it can go through Media Foundation's `IMFTransform` rather than a
   hand-rolled pipeline.

---

## 2. Encoders: the existing choice is already the best one

> **Superseded.** The frame is no longer encoded here at all. It used to be
> encoded as PNG every frame and pushed to a host-side program over RPC; it is now
> served as pixels over RFB, and the encoder is libvncserver's, running on its own
> threads. What follows is kept because it is the measurement that closed off the
> encoder as a direction — the conclusion it reaches, that the encoder was never
> the bottleneck, is exactly why handing it to a library cost nothing worth
> worrying about. See [PERFORMANCE.md](PERFORMANCE.md), section 5.

The frame path encodes the whole screen as PNG every frame, which looks like the
obvious thing to attack. It is not.

Measured on the host with a real captured frame (1024x768), 30 runs each:

| Encoder | ms/frame | Output size |
|---|---|---|
| **GDI+ PNG** (what the client uses) | **6.9** | 39,766 B |
| WIC PNG (`WindowsCodecs`) | 12.4 | 36,043 B |
| WIC BMP | 4.5 | 3,145,782 B |
| WIC JPEG q75 | 3.5 | 87,421 B |
| GDI+ `LockBits` (read pixels, no encode) | 1.2 | — |

> Measured with the host's own `gdiplus.dll` and `WindowsCodecs.dll` (10.0.19045)
> against a 10.0.19041 container. Encoder performance does not change by an order
> of magnitude between patch levels, but strictly this is not an in-container
> measurement.

Three readings:

1. **WIC PNG is 1.8x slower than GDI+ PNG.** Switching to WIC is a regression.
2. **PNG encoding is 21% of a 33 ms frame budget.** It cannot be the main cost.
3. JPEG is twice as fast but 2.2x larger and lossy, which does not pay on a LAN.

The real cost is elsewhere. The suspicion was `PrintWindow` — one cross-process
synchronous `WM_PRINT` per window per frame, a dozen-odd of them for a dialog with
a dozen controls. **That was a guess at the time; it has since been measured, and
it was right.** See [PERFORMANCE.md](PERFORMANCE.md).

---

## 3. Frame change rate

Diffing 120 exported frames pixel by pixel:

```
frames                 120
adjacent comparisons   119
identical               96  (81%)
differing               23
differing pixels        min 237 / median 2433 / max 2433
screen pixels          786,432
largest dirty area     0.309%
```

**81% of consecutive frames are byte-for-byte identical, and the worst case changes
0.31% of the screen.**

Meanwhile every frame costs a full-screen `PrintWindow`, a full-screen PNG encode,
a 40 KB RPC transfer, a full-screen PNG decode and a full-screen texture upload —
**of which 99.7% is recomputing something that did not change.**

> **What actually happened.** The measurement above held, and the direction it
> pointed at was right, but the change that took it was not on this list. The
> capture still runs every frame — section 4 of
> [PERFORMANCE.md](PERFORMANCE.md) records why event-driven skipping cannot work
> here — and what became selective is the *sending*: the captured frame is
> compared against the last one served, row by row, and only the bounding box of
> the difference is marked. The encoder, the transfer and the decode all collapse
> into that. Rows 2 and 3 of the table below became moot when the encoder stopped
> being ours.

Optimisations, in order of payoff as understood at the time:

| Approach | Expected | Cost |
|---|---|---|
| Keep the previous frame client-side, find dirty rectangles by `memcmp` per row, send only those | Two to three orders of magnitude off encode, transfer and decode on a static screen | Medium; needs IDL and both sides |
| Encode each dirty rectangle as its own small PNG | Encoded area from 786k pixels to a few thousand | Small, on top of the previous |
| `SDL_UpdateTexture` only the changed rectangle | Removes the full-screen upload | Small |
| Skip the frame entirely when nothing changed | Idle container CPU near zero | Very small, and validates the direction |

The last one is the smallest change and the quickest way to test the direction. It
was built, and it did not work — see [PERFORMANCE.md](PERFORMANCE.md), section 4.

---

## 4. The container's display: `StubDisp`, and 18 modes

**The container's resolution is not fixed at 1024x768. It is fixed to a set of 18
modes, of which 1024x768 is the first.**

Measured on `win32kbase.sys` from `windows/server:ltsc2025`. With no display
adapter, win32k falls back to a stub display driver built into itself:

```
StubDispEnablePDEV  StubDispCompletePDEV  StubDispGetModes
StubDispEnableSurface  StubDispDisableSurface  StubDispFillPath  StubDispDisablePDEV
```

`StubDispGetModes` reports 18 modes, each 32bpp at 60 Hz, and **1024x768 is first**:

| | | |
|---|---|---|
| **1024x768** | 640x480 | 800x600 |
| 1152x864 | 1280x768 | 1280x800 |
| 1280x1024 | 1440x900 | 1600x1200 |
| 1680x1050 | 1856x1392 | 1920x1200 |
| 2560x1600 | 1600x768 | 1600x900 |
| **1920x1080** | 1024x600 | **1366x768** |

`bInitGdiInfo` hard-codes the default when the DEVMODE is empty — which is exactly
the container's case:

```c
if (dmPelsWidth == 0 && dmPelsHeight == 0 && dmBitsPerPel == 0 && dmDisplayFrequency == 0)
    { dmPelsHeight = 768; v5 = 1024; }
```

`StubDispEnablePDEV` then builds the PDEV at whatever `bInitGdiInfo` accepted.

**So `ChangeDisplaySettings` with one of those 18 modes — 32bpp and 60 Hz are
required for the match — should work.** `NtUserChangeDisplaySettings` contains
only a UIPI/integrity check and no silo or container gate.

The client does this before starting the program, driven by `LAUNCHER_RESOLUTION`
(see [BUILD.md](BUILD.md), section 5). Note that the bit depth and refresh rate
have to be set as well as the size: the stub driver looks the mode up in its table
with all three attached, and silently falls back to the default when any is
missing.

---

## 5. `win32kns.sys` is not a display driver

Its FileDescription is **"Win32k non session driver"**, and it is a **WSK socket
channel driver**:

- Exports `IVInitialize`, `IVUninitialize`, `IVResolveContainerId`, `IVSend`,
  `IVRecv`, `IVBroadcast`, `IVChildReInitialize`, `IVCloseChildChannel`.
- Channel types are `mouse`, `keyboard` and `non-input`.
- Internally, `CContainerIdList::AddChannel(GUID, IV_CHANNEL_TYPE, WSK_SOCKET,
  CONTAINER_ID)`, indexed by **VmId GUID**.
- Imports `WskRegister`, `WskCaptureProviderNPI`, `NETIO.SYS`.
- Loaded by win32kbase through `\SystemRoot\System32\win32kns.sys`.

Alongside it, win32kbase has `IsVailContainer`, `NtDxgkVailConnect` and
`RIM::CompleteFrameFromContainer` / `RIM::CopyPointerFromContainer` (whose fields —
`PointerId`, `HimetricLocation`, `KeyStates` — are input, not video), and
win32kfull has `vailFrameTimes` and `ContainerInputReceiveQPC`.

**"Vail" is win32k's container input redirection stack, and `win32kns.sys` is its
transport.** That is what makes the client's `SendInput` work in the container.

### The cursor handle a container does not have

**`GetCursorInfo` returns `hCursor == NULL` in a container on essentially every
frame, while returning a perfectly good position.** Measured from the frame loop:
57 of 57 frames, then 56 of 56, across a whole session. `GetCursor` returns the
real handle on those same frames.

The two calls are not asking the same question, and nothing in the documentation
says so. Disassembled from `win32kfull.sys`:

- **`NtUserGetCursorInfo` → `_GetCursorInfo`** fills the handle from
  `[UserSessionState+36376]` — the cursor the *session is displaying*. It reads the
  position from somewhere else entirely, `LogicalCursorPosFromDpiAwarenessContext`,
  which is why the position is right while the handle is not.
- **That field is only written by `zzzUpdateCursorImage`**, which returns
  immediately if `[UserSessionState+19232]` — the session's active input queue — is
  zero, and takes the cursor to display from that queue. `zzzSetCursor` has a second
  gate in front of it: it calls `zzzUpdateCursorImage` only when the calling
  thread's queue *is* that active queue. **A container has no session, so the field
  is never populated.**
- **`NtUserGetCursor`** reads `[PtiCurrent()->pq + 424]` instead — the calling
  thread's *queue's* own cursor. `NtUserSetCursor` → `zzzSetCursor` → `LockQCursor`
  writes that one unconditionally, before the active-queue question is ever asked.

### `GetCursor` is not the way around it

`NtUserGetCursor` looks like the answer — it reads `[PtiCurrent()->pq + 424]`, the
calling thread's *queue's* own cursor, which `NtUserSetCursor` → `zzzSetCursor` →
`LockQCursor` writes unconditionally, before the active-queue question is ever
asked. It was tried, and it is worse than useless:

**It returns whatever this program's own thread last set, and this program never
calls `SetCursor`.** So it hands back the cursor win32k gives a fresh queue and
never changes — measured, a 32x32 wait cursor, 1680 bytes of it, hotspot 16,16, 348
opaque pixels, on every frame of a session, read once and then cached for ever
because the handle never differs. A window's `SetCursor` goes to *its* thread's
queue, and this thread shares no input queue with it.

### What is actually available

| Call | Reads | In a container |
|---|---|---|
| `GetCursorInfo` | the cursor the session is displaying — the active queue's | NULL on most frames (57 of 57, then 38 of 56), **and correct when it is not** |
| `GetCursor` | the calling thread's own queue's | always present, always this program's own stale one |

So the container can report the cursor only intermittently, and the rule in
`Desktop::ReadCursor` is the simple one: **take `GetCursorInfo`'s handle when it is
there, and show the standard arrow when it is not.** Nothing cleverer pays — a
grace period before the stand-in was tried and only delayed the arrow, and the
handle is either there or it is not.

**What would actually fix it** is the window under the pointer, not either call:
`WindowFromPoint` and then that window's `GCLP_HCURSOR`. It gets the I-beam over a
text field and the arrow elsewhere, which is most of what a desktop shows, and it
misses only the cursors an application sets dynamically. It is not implemented —
it is a mechanism the platform does not offer, invented to work around one it does
not maintain, and it should be a deliberate choice rather than a fallback.

---

## 6. RDP: the whole server stack is present and cannot run

The image contains a complete RDP server stack — `termsrv.dll`, `rdpcorets.dll`,
`rdpbase.dll`, `rdpdr.sys`, `rdpvideominiport.sys`, `umrdp.dll`, `umb.dll` and the
rest — and it does not work, for architectural reasons rather than configuration.

**This was the longest part of the investigation and has its own document:
[RDP-IN-CONTAINER.md](RDP-IN-CONTAINER.md).** In one line: a process-isolated
container's object namespace does not contain the anonymous device objects PnP
enumerates, so the RDP input stack cannot open a UMBus device interface and gets
`ERROR_FILE_NOT_FOUND`.

---

## 7. Audio: no endpoint, and RDP is not a way around it

Containers have no audio endpoint and no stream to capture. `rdpsnd.dll`,
`rdpwd.sys` and `tsaudio` are absent, and nothing under `System32` has audio, snd,
mixer or wave in its name.

Since RDP itself cannot run in the container (section 6), "use RDP to get audio"
is not available either.

**See [AUDIO.md](AUDIO.md)** for the measurements and for what faking an endpoint
would take.

---

## 8. Miscellaneous

- **`wmsgapi.dll`** exports `WmsgBroadcastMessage`, `WmsgPostMessage`,
  `WmsgSendMessage` and `WmsgSendPSPMessage`. It is how the container host sends
  window messages across a container boundary (for WSL and container integration).
  Not usable from *inside* a container.
- **`computecore.dll`, `computenetwork.dll`, `computestorage.dll`** are HCS
  clients, useful to tools running in a container, not to this project.
- **`win32kfull.sys`** has container-mode symbols such as `IsHvsiContainer` and
  `GetSynthesizedContainerMouseInputEvent`, but nothing configurable for a
  container's virtual display size.
- **Window station**: `CreateWindowStation("WinSta0")`, `CreateDesktop("Default")`,
  `SetUserObjectInformation(WSF_VISIBLE)` and `SwitchDesktop` in `Desktop.cpp`. This
  works in the container and is what makes the whole thing possible.

---

## 9. Summary

| Question | Answer |
|---|---|
| Is there a better way to capture than `WM_PRINT`? | **No.** No DWM, no WARP (on 20H2), no magnifier; GDI is the only route |
| Would WIC encode faster? | **No, 1.8x slower.** GDI+ PNG is already the best available |
| Is PNG the bottleneck? | **No**, 21% of the budget. The bottleneck is `PrintWindow`, now measured |
| Where is the headroom? | Not in dirty rectangles, as it turned out. See [PERFORMANCE.md](PERFORMANCE.md) |
| Could the whole approach be replaced? | **No.** The RDP server stack is complete and `TemporaryALiC` lifts the first barrier, but the protocol stack depends on UMBus and PnP enumeration, which a process-isolated container cannot provide. See [RDP-IN-CONTAINER.md](RDP-IN-CONTAINER.md) |
| Audio? | None at all, and not even through RDP redirection. See [AUDIO.md](AUDIO.md) |
| Can the resolution be changed? | **Yes, within limits.** The container's display is win32k's `StubDisp`, which advertises 18 modes; `LAUNCHER_RESOLUTION` picks one. Sizes outside the list are refused |
| The biggest lesson? | **"An empty event log" does not mean "the code did not run."** With diagnostics off by default, everything looks fine |
