# Audio: feasibility

**There is no audio endpoint in the container and no stream to capture.** The
only route that could work is faking an endpoint in user mode, which is a
separate project rather than a module added to this one.

Recorded here so it does not have to be investigated twice.

---

## 1. Measured

Inside a Server Core container:

```
waveOutGetNumDevs()                        -> 0
Get-CimInstance Win32_SoundDevice          -> empty
Get-Service Audiosrv, AudioEndpointBuilder -> empty (services do not exist)

C:\Windows\System32\MMDevAPI.dll           -> present
C:\Windows\System32\audioses.dll           -> absent
C:\Windows\System32\winmm.dll              -> present
C:\Windows\System32\dsound.dll             -> absent
C:\Windows\System32\XAudio2_9.dll          -> absent

HKLM\SOFTWARE\Classes\CLSID\{BCDE0395-E52F-467C-8E3D-C4579291692E}  -> registered
    \InprocServer32                        -> present
```

`{BCDE0395-E52F-467C-8E3D-C4579291692E}` is `MMDeviceEnumerator`.

---

## 2. Why it cannot be filled in

Video and audio depend on entirely different machinery:

| | Video | Audio |
|---|---|---|
| Depends on | Win32k's virtual desktop | An endpoint provided by a kernel audio driver |
| In the container | Win32k is present; the client creates its own WinSta0 | A separate device namespace — **no hardware, no endpoint** |

Specifically:

- **Process-isolated containers share the host kernel**, so installing a driver
  means installing it on the host, which the container platform refuses.
- **Hyper-V-isolated containers run their kernel in a utility VM** managed by
  Docker, which is not reachable.
- Windows has no concept of a null audio device. `waveOutGetNumDevs()` returns 0
  on a machine with no hardware, by design.
- The image's component set is fixed at build time. `Install-WindowsFeature`
  cannot add audio components, and would not help if it could — there is still
  no device.

So audio APIs fail outright in the container: `GetDefaultAudioEndpoint` returns
`E_NOTFOUND`, `waveOutOpen` returns `MMSYSERR_NODRIVER`. **There is no stream to
capture.**

---

## 3. The only route: fake an endpoint in user mode

Rather than capturing a device's output, make programs believe a device exists,
intercept the PCM they write into its buffer, and send it to the host to play.

### Interfaces that would have to be implemented

`audioses.dll` being absent means **hooking the enumeration layer alone is not
enough**: even if enumeration succeeded, `IMMDevice::Activate` could not produce
an `IAudioClient`, because that lives in the AudioSes service, which does not
exist. The whole chain has to be implemented:

```
IMMDeviceEnumerator   GetDefaultAudioEndpoint, EnumAudioEndpoints, GetDevice,
                      Register/UnregisterEndpointNotificationCallback
IMMDevice             Activate, OpenPropertyStore, GetId, GetState
IAudioClient          Initialize, GetBufferSize, GetMixFormat, IsFormatSupported,
                      GetDevicePeriod, GetCurrentPadding, Start, Stop, Reset,
                      SetEventHandle, GetService, GetStreamLatency
IAudioRenderClient    GetBuffer, ReleaseBuffer
```

About 30 methods. `QueryInterface` also has to answer for `IAudioClient2` and
`IAudioClient3`, which many applications ask for.

**The clock is the part most likely to be underestimated.** With no real device
there is no hardware clock, and the value returned by `GetCurrentPadding` is what
paces the application's writes. It has to be derived from the sample rate, or
applications will run away or stall.

### Delivery: replace the COM registration, no injection

`MMDeviceEnumerator`'s CLSID is registered to `MMDevAPI.dll`. **The container has
no audio anyway, so repointing that `InprocServer32` at a shim DLL is lossless:**

```
HKLM\SOFTWARE\Classes\CLSID\{BCDE0395-E52F-467C-8E3D-C4579291692E}\InprocServer32
    (Default) -> path to the shim DLL
```

Any process in the container calling `CoCreateInstance(CLSID_MMDeviceEnumerator)`
then loads the shim, with **no `SetWindowsHookEx`, no Detours, no injection**. Both
32- and 64-bit builds are needed.

### Layers that do not need covering

`dsound.dll` and `XAudio2_9.dll` are absent, so anything depending on them cannot
play audio in the container in the first place. `winmm.dll` is present with zero
devices, so `waveOutOpen` fails; shimming it is an optional extra and much
simpler than WASAPI, being a plain C API.

---

## 4. Rough cost

| Part | Estimate |
|---|---|
| WASAPI shim (4 interfaces, COM registration swap, format negotiation, invented clock) | 1–2 weeks |
| waveOut shim (plain C API) | 2–3 days, optional |
| Transport (RFB extension, audio thread, viewer-side playback) | 3–5 days, **the certain part** |

The transport is the same problem it was, on a different protocol: RFB has
extension points for exactly this, and libvncserver has the hooks for a custom
pseudo-encoding. The real uncertainty is still the shim's clock and format
negotiation.

> Written when the transport was RPC, so it said "IDL extension" and "host-side
> SDL playback". Both are gone; the shape of the estimate is not.

**The program would need a second thread.** It is currently a single capture loop,
with libvncserver's own threads doing the encoding and the sockets. Audio capture
is callback-driven and needs its own thread and queue.

---

## 5. When not to do it

- The existing use cases — installing a GUI application, exporting an image,
  testing a suspicious program in a lightweight sandbox — **none need sound**.
- If the goal is watching video or listening to audio, **a container is the wrong
  vehicle**. VM audio passthrough is far more mature.
- **RDP is not an alternative.** The image contains a complete RDP server stack,
  but it cannot start in a container (the protocol stack depends on UMBus and PnP
  enumeration; see [IMAGE-ANALYSIS.md](IMAGE-ANALYSIS.md)). This image also has no
  `rdpsnd.dll` — nothing under `System32` has audio, snd, mixer or wave in its
  name. Both routes are closed.

---

## 6. If it is done, start with a de-risking spike

**Do not build the transport first.** Write the smallest spike that tests the core
assumption: put a shim DLL in the container, replace the `MMDeviceEnumerator` COM
registration, and write a few lines of WASAPI playback as a target. Confirm it can

1. obtain a "default device",
2. `Initialize` successfully, and
3. receive **continuous, correctly paced** PCM in `GetBuffer`.

**If that passes, the rest is worth building; if it does not, the shim is more
complex than estimated and the work should stop there.** About a day, and it is
the only genuinely uncertain part.
