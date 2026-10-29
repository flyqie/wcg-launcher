# Performance: where a frame's time goes

Measured inside a process-isolated container, with the frame profiler built into
`Launcher.exe` (`Source/Remote/Server.cpp`). Not estimates — set `LAUNCHER_DEBUG=1` and it
prints a breakdown once a second.

Architecture is in `README.md`; building and running in [BUILD.md](BUILD.md);
what is actually in the container image in [IMAGE-ANALYSIS.md](IMAGE-ANALYSIS.md).

> **The transport changed after most of this was measured.** The frame used to be
> encoded to PNG and pushed to a host-side program over RPC; it is now served as
> pixels over RFB by libvncserver. That removes the `encode` and `send` phases
> from the loop entirely, and with them the `toGdi+` part of the capture — the
> alpha pass and the GDI+ wrap existed only to feed the PNG encoder.
>
> Everything measured about the **capture** still holds: `Desktop::Capture` and
> `DesktopCapture.cpp` were not touched. Where a number below includes `toGdi+`,
> it is marked, because that part of it is no longer paid.

---

## 1. A frame has five phases

| Phase | Contents |
|---|---|
| `capture` | `Desktop::Capture()` and `Desktop::Finish()`: the full-screen bitmap, the desktop fill, `PrintWindow` over every window |
| `dock` | `Dock::Draw()` |
| `cursor` | `Desktop::ReadCursor()`, plus publishing a shape when it changed |
| `input` | Draining what the viewers asked for and injecting it into the desktop |
| `diff` | The row-by-row comparison against the framebuffer last served, and the copy of what changed |

`capture` is broken down further:

| Part | Contents |
|---|---|
| `alloc` | `CreateCompatibleBitmap` for the full-screen target |
| `fills` | `FillRect` to the desktop colour |
| `windows` | `EnumDesktopWindows` plus every `PrintWindow` and `BitBlt` |

`windows` is split again into `toplevel` (the `PrintWindow` on each top-level
window) and `child` (the `PrintWindowTree` re-print of descendants), each reported
with a per-call cost.

**The dock and the cursor are reported on their own lines**, not inside `capture`:
the dock is drawn onto the frame by `Dock::Draw` between the capture and the
finish, and the cursor is a separate query against the desktop. Neither is a
capture cost, and the capture is not told about either.

**What is not here is the encoder.** libvncserver encodes and sends on its own
threads, and this program never sees those start or finish. `diff` is what
replaced the old `encode` and `send` columns, and it is a much smaller number —
see section 5 for what that does and does not mean.

### The dock

Newer than the measurements in section 2, which were taken before it existed, and
only there at all when the program is run with `LAUNCHER_DOCK=1`. It does not scale
with the screen or with the number of controls on it.

Measured over 21 runs on an idle `cmd` console, on a 1024x768 desktop:

| | us |
|---|---|
| first frame | 904 (one-off: the brushes and font are created here) |
| mean | **~277** |
| range | 211–398 |

About 0.8% of a 33 ms budget, or 3% of an idle frame — the same order as `fills`,
an order below `windows`. It is a **per-frame** cost, so it scales with the frame
rate: 1.6% of a core at 60 fps.

The enumeration in `Refresh` is the suspected bulk of it. `Desktop::Capture`
enumerates the desktop's top-level windows anyway, a few lines earlier, so handing
that list across to `Dock::Draw` would remove it — but that would put a desktop
detail into the dock's interface for 0.8% of a budget, on an opt-in switch, and it
has not been worth it.

---

## 2. Measured

Taken with the PNG-and-RPC transport, so **`toGdi+` is included and is no longer
paid**, and `encode`/`send` do not exist any more. The capture columns are the
ones still worth reading.

### Idle (a cmd console, one `PrintWindow` call)

| | us | share of the 33 ms budget |
|---|---|---|
| `alloc` | 368 | 1.1% |
| `fills` | **4** | 0.01% |
| `windows` | 4427 | 13% |
| `toGdi+` | 2554 | 7.7% — **gone** |
| **capture** | **8634** | **26%** (about 6080 without `toGdi+`) |
| encode | 1178 | 3.6% — **gone** |
| send | 1472 | 4.5% — **gone** |
| **total** | **11284** | **34%** |

### With a dialog open (15 `PrintWindow` calls)

| | us | share of the 33 ms budget |
|---|---|---|
| `alloc` | 366 | 1.1% |
| `fills` | **4** | 0.01% |
| `windows` | **16170** | 49% |
| `toGdi+` | 1222 | 3.7% — **gone** |
| **capture** | **18003** | **55%** (about 16780 without `toGdi+`) |
| encode | 8040 | 24% — **gone** |
| send | 5422 | 16% — **gone** |
| **total** | **31465** | **95%** |

### What that means

**Capacity: about 3 containers per core idle, about 1 with a dialog open** — for
the capture loop. It is now a floor rather than the whole answer, because
libvncserver's encoding runs on top of it and is not counted here.

**`windows` is the largest single part** — 51% of capture idle, 90% with a dialog.

**Capture cost is linear in the `PrintWindow` call count**, at roughly
`8.1 ms + 0.72 ms x calls`:

| calls/frame | capture mean | |
|---|---|---|
| 1 | 8.6 ms | a full-screen cmd console |
| 4 | 10.4 ms | |
| 15 | 18.9 ms | a dialog with a dozen children |

Per call: **4427 us** for the full-screen console, **82–121 us** for a dialog's
child controls. So it is roughly "1 ms fixed plus something proportional to area".

---

## 3. Three A/B experiments

Each experiment ran behind an environment-variable switch, so none of them needed
a rebuild to compare. **All of those switches have since been deleted**, along with
section 4's desktop-skip, capture-timing and no-paint-probe ones — all of them
named after the project's old prefix, which no longer exists anywhere. What they
measured is recorded below; nothing in the shipped program is configurable except
`LAUNCHER_VNC`, `LAUNCHER_VNC_PORT`, `LAUNCHER_VNC_PW`, `LAUNCHER_RESOLUTION`,
`LAUNCHER_DOCK`, `LAUNCHER_FPS` and `LAUNCHER_DEBUG`.

### Experiment A: take the cursor out of the frame — **worked, merged**

`DrawCursor` painted the cursor into the composite. Measured, that cost
**1.2–1.5 ms per frame** on an idle screen, from `GetIconInfo` allocating two
bitmaps per call plus `DrawIconEx` and two `DeleteObject` calls.

| | before | after |
|---|---|---|
| capture (idle) | 8634 | **6379** |
| mean frame | 11284 | **9225** |
| capacity | ~3/core | **~3/core (18% less CPU)** |

**The stronger reason has nothing to do with CPU**: a cursor is not a window, so
**moving it raises no window event**. While the cursor was baked into the frame,
mouse movement changed the pixels with nothing to signal it — which would have
made any event-driven capture skip miss it entirely.

The fix: the cursor travels beside the frame instead of inside it, and **the
viewer composites it**. RFB carries it as a cursor shape and a pointer position,
which is the same split the RPC interface had; what changed is who draws it.

### Experiment B: bare `WM_PRINT` instead of `PrintWindow` — **failed**

The hypothesis was that `PrintWindow` sends `WM_PRINT` with `PRF_CHILDREN`, which
recurses into the whole subtree, and that `PrintWindowTree` then paints it again —
duplicated work.

The numbers looked excellent, but **the frame came back as nothing but the desktop
fill, with no windows at all**:

| | before | bare WM_PRINT |
|---|---|---|
| toplevel | 3173 us/call | **788 us/call** |
| capture | 8634 | **3538** |

Two conclusions, the second of which corrects the reading of the first:

1. **The approach does not work.** An HDC is only meaningful in the calling
   process, so a plain `SendMessage(WM_PRINT)` across processes does not paint.
   `PrintWindow` exists precisely to make the cross-process case work — it is
   backed by win32k, not by a user-mode `SendMessage`.
2. **The speedup was an artefact.** `PrintWindow` at 3000 us was *painting*;
   `SendMessage` at 800 us was *painting nothing*. The 2200 us difference is the
   cost of painting at all.

**So the window pass is close to the real cost of painting the tree, and there is
no redundant pass to remove.** The switch stays in the code only to record the
result, so it is not tried again.

### Experiment C: skip the `PrintWindowTree` re-print — **not worth it**

| | before | re-print off |
|---|---|---|
| capture (dialog) | ~18000 | ~17000 |
| picture | correct | status bar loses its parts, buttons flicker |

**About 1.1 ms saved, at the cost of flicker.** The re-print itself is cheap
(82–121 us per call); it exists because `PrintWindow`'s `PRF_CHILDREN` pass is
unreliable for themed common controls. 1 ms is not worth the flicker.

---

## 4. Event-driven frame skipping: **tried, does not work here**

With `capture` proving irreducible, the remaining lever was **capturing less
often**. Measured, 81% of consecutive frames are byte-for-byte identical, and
every one of them paid for a full capture before being discarded.

**Conclusion: neither change detector fires in a process-isolated container. The
code has been deleted.**

### Detector 1: `SetWinEventHook`

```cpp
SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_END, NULL, WinEventProc,
                0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
```

It installed without error and its callback ran on a dedicated message-pumping
thread (which `WINEVENT_OUTOFCONTEXT` requires).

**Across three runs, while windows were opened, dragged and typed into, it
received zero events.** The `change events` diagnostic line never printed once.

### Detector 2: `GetUpdateRect` per window

Orders of magnitude cheaper than a `WM_PRINT` (**~80 us per frame**, against
7500 us for a capture), and driven by the same invalidation the window's own
painting uses.

**`paint` in the `capture reasons` breakdown was always 0** — no window ever
reported a pending update region.

### The consequence

With both silent, the skip degenerated to the one-second resync alone:

```
capture reasons  none=0 forced=0 event=0 paint=0 baseline=0 resync=2
```

**The picture updated once a second**, which reads as unusable lag, and the cost
it saved was not worth that.

### Why the code was deleted rather than kept disabled

This project is **for process-isolated containers only**. Both detectors are silent in
the one environment that matters, so "it might work elsewhere" is not a reason to keep
dead code — it would only lead the next person to try it again. This section has the
measurements, where the hook was installed, and why it needed its own message-pumping
thread, which is what a retry would need.

**What it bought instead: capture every frame at 30 fps = 30% of one core = 3
containers per core**, with no heuristics and nothing that can silently stop working
for a class of application. A 16-core host is about 48 containers, before the encoder.

### What did change

The *sending* is now selective even though the capturing is not. `Frame::Update` compares
the captured frame against the framebuffer last served and marks only the rectangles that
cover what differs, so a frame that captures the whole screen costs the network almost
nothing when only a caret moved — the same win dirty rectangles were meant to buy,
without needing a change detector.

**Measured on 4000 randomised screens**, scattered changes of every size and position,
odd widths included: every changed pixel was covered by some rectangle, and the marked
area came to **3.3× the pixels that actually changed** — against **8.9×** for a single
bounding box over everything. The encoder and the link pay for the marked area, so that
is the number that moved. The cost of the comparison itself is reported as `diff`, and
the area it marks as `marked`.

### What did work: backing off on a still screen

The detectors above failed, but there is a signal that cannot fail, because it is not a
detector: **the comparison the loop already does**. It knows how much of the screen
changed, and a frame in which almost nothing changed does not need the next one at full
rate.

So the interval doubles while the marked area stays under 1% of the screen — up to three
doublings, an eighth of the rate — and returns to one when a larger change is seen.
`LAUNCHER_FPS` stays the ceiling.

Input wakes the loop directly and is applied without waiting for a capture. It also
resets the back-off and advances the next capture, but never closer than one base frame
interval after the previous capture. A 500 ms active window keeps small changes such as
typing at the base rate instead of immediately backing off again. Autonomous changes
still wait until the next idle capture. The rate the profile prints is the rate
actually achieved:

```
--- frame profile: 12 frames in 2016 ms (6 fps), 2 with changes, 1 viewer(s), 4 kB sent ---
```

Six, not thirty, on a screen that is not moving. On an idle desktop that is most of a
core saved, and it costs nothing a person can perceive.

---

## 5. What the profile cannot time, and how it bounds it

This is the one thing to be careful about when reading the numbers.

| | where it runs | counted here? |
|---|---|---|
| Capture, dock, cursor, input, diff | this program's thread | **yes**, per phase |
| Encoding, sockets, per-client updates | libvncserver's threads | **no phase**, but see below |

So `mean frame … => ~N containers per core` is a floor. The real figure is lower,
by however much libvncserver spends turning changed regions into RFB rectangles.

**The profile now says how much lower.** It reports CPU time for this thread and for
the whole process, and the difference is every other thread's:

```
  cpu us/frame  this thread 5761, whole process 7400, so 1639 elsewhere (the encoder and the sockets)
```

`GetThreadTimes` and `GetProcessTimes`, differenced over the window — two calls per
report, and the only way to see libvncserver's threads from inside this program. It
counts kernel time as well as user, so it includes the socket sends; it cannot say
*which* part is the encoder, only what all of it costs together.

It also reports what actually left the container, from libvncserver's own per-client
counters:

```
--- frame profile: 128 frames in 4270 ms, 96 with changes, 1 viewer(s), 412 kB sent ---
```

**That number is the honest one for the network question**, and it is a different
kind of number from the old `send` column. The old one was a synchronous round
trip this program waited on, including the host's PNG decode and texture upload —
about 4.9 ms of a 33 ms budget, and the reason several containers sharing one
server stalled each other. There is no equivalent now: `rfbMarkRectAsModified`
returns as soon as the region is recorded, and a viewer that cannot keep up
accumulates a larger region rather than stalling the capture loop.

> The old `send` measurement is kept in the table in section 2 because it is what
> motivated the change, not because it is comparable.

### Frames that are not taken

With no viewer connected the loop skips the capture entirely and sleeps, which is
most of a core saved on an unwatched container. Those frames are counted and printed
rather than silently dropped, because otherwise the report would look like the loop
had stalled:

```
  skipped 40 frames, nothing connected to send them to
```

They are deliberately **not** samples. A skipped frame has no cost, and averaging
zero-cost frames into the means would describe a loop that is not the one running.

---

## 6. Frame rate: `Sleep` quantisation

Before the fix, frame time measured 41–46 ms (22–24 fps) against a configured
33 ms. The cause: `Sleep` is quantised to the system timer tick, 15.6 ms by
default. Idle, the work was 11.1 ms, so `Sleep(21.9)` became two ticks (31.2 ms)
for a total of 42.3 ms; on a heavy frame the work alone was 30.9 ms and
`Sleep(2.1)` still became one whole tick, for 46.5 ms. Both match the measurement.

With `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ...)`,
measured frame time dropped to **35–39 ms (26–28 fps)**. The remaining ~3 ms is
`WaitForSingleObject` scheduling latency in the VM.

### The schedule is fixed, not measured from the end of the frame

`LAUNCHER_FPS` is a period, not a gap. The next frame is one interval after the one
that was *due*, not one interval after this frame finished — because pacing from the
end of the frame makes the period "the work plus the interval", and that is not a
rate control:

| | period | at `LAUNCHER_FPS=30`, 6 ms of work |
|---|---|---|
| From the end of the frame | work + interval | 39 ms → **25 fps** |
| Fixed schedule | interval | 33 ms → **30 fps** |

Measured, the first of those is what the loop did: the profile reported 25–26 fps
against a setting of 30, on every window, and the gap was exactly the frame's own
cost.

**A frame that overruns its slot resynchronises rather than catching up.** If the
work takes longer than the interval, the schedule falls into the past and the next
frame is due immediately — so the loop runs as fast as the work allows and the
profile reports a rate below the setting. Running a burst of frames to make up the
lost ones would spend the CPU the setting exists to bound.

The achieved rate is printed in the profile's header line, which is how either case
is told apart: at the setting means the frames fit, below it means they do not.

### The wait now wakes for input as well

The timer is one of two things the loop waits on. The other is an event the input queue
sets, so that a click is applied when it is made rather than at the next frame boundary —
which matters because injection has to happen on the thread that is on the container's
desktop, and that thread is this loop.

**Input cuts the wait short; it does not postpone the frame.** Two things would otherwise
go wrong, and both are guarded in the loop: if input merely woke the loop and the loop
then took a frame, the capture rate would follow the mouse; and if input kept arriving,
the loop would go round draining for ever without ever reaching the capture. So the wake
comes back round, drains, and waits again — unless the frame has fallen due, in which
case it is ignored and the frame is taken.

The result is a capture rate that is still exactly `LAUNCHER_FPS` and input applied
within a scheduling quantum of arriving, instead of within a frame.

---

## 7. Tried and did not work

Recorded so they are not retried:

| Attempt | Result |
|---|---|
| **`PrintWindow` with `PRF_CLIENT\|PRF_NONCLIENT` instead of 0**, to drop the `PRF_CHILDREN` recursion and stop painting every descendant twice | **`PrintWindow` returns FALSE.** Measured against twelve real windows: flags `0`, `PW_CLIENTONLY` and `PW_RENDERFULLCONTENT` are all this API accepts now; every `PRF_*` combination fails outright. The old flags are `WM_PRINT`'s, and `PrintWindow` no longer honours them. The descendant re-print stays, and the double painting with it |
| **Printing straight into the target with the window's rect origin as the viewport origin**, to drop the scratch bitmap and the `BitBlt` per window | **`PrintWindow` ignores the viewport origin** and paints at the DC's corner. Measured: a window at `766,200` came out 130,753 pixels different from the scratch-and-copy path, while one at `-7,-7` came out identical -- the error is exactly the window's position. The scratch and the `BitBlt` are load-bearing |
| **Dropping the `FillRect` before each `PrintWindow`**, since flags=0 erases the background itself | **It does erase it** -- measured, 11 of 12 windows left none of a sentinel colour behind -- but the twelfth left 1.5%, which is a window with a region or a hole. The `FillRect` is what those show instead of the previously captured window's pixels. Kept |
| Bare `WM_PRINT` instead of `PrintWindow` | **Empty picture**; see experiment B |
| Skipping the `PrintWindowTree` re-print | Saves 1.1 ms and brings back flicker |
| Deleting the redundant full-screen `Rectangle()` fill | Saves **4 us**. GDI takes a fast path and `fills` was already only 4 us — the whole step is not a cost |
| Caching the full-screen bitmap | **Not done.** Low priority by the numbers: `alloc` is 366 us |
| Dirty rectangles | **Done**, but on the sending side rather than the capture side: `diff` marks only what changed, so the network cost is proportional to the change while the capture cost is not. See section 4 |
| Skipping capture when no viewer is connected | **Done.** Most of a core saved on an unwatched container |
| **Reusing one scratch DC for every window**, instead of `GetDCEx` + `CreateCompatibleDC` per window | **Done**, and the only part of the window pass that turned out to be removable: the DC exists to hold the scratch bitmap and nothing about it depends on which window is being painted |

---

## 8. An anomaly that is explained, and one that is not

Early runs showed `cursor` and `toGdi+` varying by more than an order of magnitude
between scenes:

| calls/frame | `cursor` | `toGdi+` |
|---|---|---|
| 1 (idle) | 806–1105 | 2329–2870 |
| 15 (dialog) | **66–91** | **1232–1315** |

while `alloc` (351 vs 362) and `fills` (4 vs 4) were identical, which rules out a
global effect.

**The explanation is CPU frequency scaling.** A later run made it obvious: the
same `Desktop::ReadCursor` code measured **41–64 us when the loop was mostly idle and 13 us
when it was busy**. An idle loop lets the CPU downclock, so the few operations
that do run are slower each; `alloc` and `fills` are dominated by fixed GDI costs
rather than CPU-bound loops, which is why they did not move.

This means **the "1.2–1.5 ms cursor cost" is inflated by the idle clock** — the
work is real, since the A/B proved it, but the absolute figure varies with
frequency.

**What is still not explained** is why `toGdi+` differed between the two scenes at
all. That path is gone with the GDI+ dependency, so it is recorded rather than
investigated.

---

## 9. Reproducing

A normal run prints a block once a second, provided `LAUNCHER_DEBUG=1` is set:

```
docker run --rm -ti --name desktest --isolation process ^
  -e LAUNCHER_DEBUG=1 ^
  -e LAUNCHER_VNC=1 ^
  -p 5900:5900 ^
  -v C:\path\to\Launcher.exe:C:\Launcher\Launcher.exe:ro ^
  mcr.microsoft.com/windows/server:ltsc2025-amd64 ^
  C:\Launcher\Launcher.exe
```

The experiments above no longer have switches to flip, so reproducing one means
re-introducing it. Their measurements stand on their own.

**Measure three scenes**, because costs differ enough that one gives the wrong
answer:

| Scene | Use |
|---|---|
| Empty desktop | The floor |
| `cmd.exe` console | The cost of one full-screen window |
| **A dialog with a dozen children** | **The ceiling**; open a property sheet with `rundll32` |

Read the `worst frame` line and the `=> ~N containers per core` line — and
remember section 5: they cover the capture side, not the encoder.
