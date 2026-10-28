# Running an RDP server in a Windows process-isolated container: root cause

> The short version: **a process-isolated container's object namespace does not
> contain the anonymous device objects that PnP enumerates**, and the RDP server's
> input stack has to open a UMBus device interface. It cannot, so it gets
> `ERROR_FILE_NOT_FOUND`. This is a container boundary — the image, the DLLs and
> the registry cannot change it.

This is a complete trace of how that was established, with each discarded
hypothesis and the experiment that discarded it. The method transfers directly to
any "something device- or driver-related does not work in a container" problem.

---

## 1. Symptom

In a `mcr.microsoft.com/windows/servercore:20H2` container (`--isolation process`),
after starting TermService:

- 3389 does not listen at all, and the event log says nothing.
- With a set of binary patches applied, 3389 listens, but a client connecting
  **hangs at "configuring the remote session"** and disconnects after about 20
  seconds. On the server, `qwinsta` shows a session that flashes past.

## 2. Environment

| | |
|---|---|
| Host | Windows Server, version 20H2 (build 19042) |
| Container | `mcr.microsoft.com/windows/servercore:20H2`, `isolation=process` |
| Image layers | base 2020-09-11 + cumulative update 2022-08-05 |
| Client | `mstsc` on Windows 10 |

**The key premise**: a process-isolated container shares the host kernel, so its
kernel drivers are the host's.

---

## 3. First barrier: the container check

`termsrv.dll` checks for a container on the listener-creation path. Detecting one
abandons the attempt and returns `ERROR_FILE_NOT_FOUND`, **writing nothing to any
log**.

Decompiled, the check reads:

```c
RegQueryValueExW(hKey, L"TemporaryALiC", ...);   // HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server
```

**When it is non-zero the container check returns 0** and the barrier is gone.
The name is "Temporary **A**llow **Li**stener in **C**ontainer" — a test switch
Microsoft left in.

```cmd
reg add "HKLM\System\CurrentControlSet\Control\Terminal Server" /v TemporaryALiC /t REG_DWORD /d 1 /f
```

With it set, the barrier really is lifted. This is the mechanism from Rafael
Rivera's 2018 article, **and it still works**.

---

## 4. Second barrier: the listener still does not start

With the first barrier lifted, capturing `termsrv`'s WPP output shows:

```
:Listener RDP-Tcp terminal class (null)
:Creating listener RDP-Tcp
ERR::CoCreateInstance( IWRdsProtocolManager ) failed: 0x80070002 in CProtocolManager::Initialize
ERR::ptrProtocolMgr->Initialize failed: 0x80070002 in CProtocolExMgr::CreateListener
:Protocol failed to create listener RDP-Tcp, error 0x80070002
ERR::No listeners
```

### How to see that output

`_DbgPrintMessage` has all three of its exits closed by default (the ETW provider
level is 0, `g_bDebugSpew` is hard-coded to 0, and nothing reads the in-memory ring
buffer). Seeing it requires starting an ETW session:

```powershell
# DebugFlags/DebugLevel are read by CWPPLoggerGuidConfig::ReadTraceConfig
New-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server' -Name Debug      -PropertyType DWord -Value 1          -Force
New-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server' -Name DebugFlags -PropertyType DWord -Value 0xFFFFFFFF -Force
New-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server' -Name DebugLevel -PropertyType DWord -Value 0xFFFFFFFF -Force

# Note the level is a UCHAR (0-255); 0xFFFFFFFF reports "Out of present range."
logman start WcvS1 -p '{5283D5F6-65B5-425F-A30B-F16C057D6B57}' 0xFFFFFFFF 255 -o C:\s1.etl -ets
```

**Encoding a WPP provider GUID**: the hex in a symbol name
`WPP_<32hex>_Traceguids` **is the GUID's string form** — insert the hyphens as
`8-4-4-4-12`. **Do not run it through `bytes_le` as well**; that reverses it twice
and lands on the wrong provider. (That mistake cost a round: zero events captured,
because the chosen GUID belonged to `CUMRDPListenerVMBus`, which has nothing to do
with the failure.)

### Following `0x80070002` down

`0x80070002` is `HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)`, passed through
unchanged from `CUMRDPProtocolManager::FinalConstruct`:

```
termsrv  CProtocolManager::Initialize
  └─ CoCreateInstance({5828227c-20cf-4408-b73f-73ab70b8849f} /*IWRdsProtocolManager*/)
       └─ rdpcorets!CUMRDPProtocolManager::FinalConstruct
            └─ CTiBus::Initialize(L"TERMINPUT_BUS")
                 └─ CTiBus::SetupDevice(this, this, L"TERMINPUT_BUS", ...)
                      └─ umb.dll!CUMBusDriver::AddBusEnumerator
                           ├─ CUMBusDeviceInterface::OpenRootUmBusEnumerator   <- fails here
                           └─ CUMBusDriver::CreateBusEnumerator
                                └─ DeviceIoControl(hUMBus, 0x228000, <DWORD 1><L"TERMINPUT_BUS">, ...)
```

`CTiBus::Initialize` has four failure points, and only the
`OpenRootUmBusEnumerator` / `CreateBusEnumerator` path can produce `0x80070002`
(the failure path inside `CreateBusEnumerator` is `GetLastError() | 0x80070000`,
and `GetLastError() == 2` gives exactly that).

### A temporary bypass (to keep analysing, not a fix)

Letting `FinalConstruct` ignore `CTiBus::Initialize`'s failure gets the listener up:

```
rdpcorets.dll file offset 0x16617   8b f0  (mov esi,eax)   ->  31 f6  (xor esi,esi)
rdpcorets.dll file offset 0x16619   85 c0  (test eax,eax)  ->  31 c0  (xor eax,eax)
```

With those, 3389 listens, `qwinsta` shows `rdp-tcp 65536 Listen`, and sessions can
be created (`csrss` starts, `CsrConnected` is reached).

---

## 5. Third barrier: the session hangs at "configuring the remote session"

A client connects, hangs for about 20 seconds and disconnects. `termsrv`'s WPP
output:

```
[termsrv] :Assign session id 33 to terminal Trace ID 0x35
[termsrv] ERR::this->ptrIWRdsProtocolConnection->NotifySessionId failed: 0x80070102 in CConnectionEx::AssignSession
[termsrv] ERR::AssignSession failed: 0x80070102 in CRemoteTerminal::DoConnect
```

`0x80070102` is `HRESULT_FROM_WIN32(258 /*WAIT_TIMEOUT*/)`. In `rdpcorets.dll` it
has **exactly one producer**, and it is a literal:

```c
__int64 __fastcall CTiDevice::WaitForDeviceReady(HANDLE *this)
{
  bool v2; // zf
  __int64 result; // rax

  v2 = WaitForSingleObject(*(this + 13), 0x4E20u) == 258;   // 20000 ms
  result = 2147942658LL;                                     // = 0x80070102
  if ( !v2 )
    return *((unsigned int *)this + 28);
  return result;
}
```

The call chain:

```
CUMRDPConnection::NotifySessionId
  └─ CUMTSInputPlugin::CreateInputDevices
       └─ CTiDevice::WaitForDeviceReady
```

That "device ready" event is created by `CreateEventW` in `CTiDevice::Initialize`
and is **only ever signalled by `CTiDevice::SignalReady`, on `CTiBus::SetupDevice`'s
success path**.

**So both barriers have one root cause**: `CTiBus` cannot be built, which (1) fails
`FinalConstruct` during listener creation and (2) leaves the device-ready event
unsignalled during session setup.

---

## 6. Ruling out a binary version difference

The first hypothesis was that a Microsoft image update had changed
`rdpcorets.dll`. **It was wrong, but the experiment that ruled it out was worth
doing.**

The key finding: **the host's `rdpcorets.dll` and `umb.dll` are byte-for-byte
identical to the ones in the image's base layer**:

| File | Host | 20H2 base layer | 20H2 CU layer |
|---|---|---|---|
| `rdpcorets.dll` | 1611776 · `802AD462…` | **1611776 · `802AD462…`** | 1639936 · `5C14BFA2…` |
| `umb.dll` | 78848 · `E28A994E…` | **78848 · `E28A994E…`** | 78848 · `A0EE4FA8…` |

The host is Server 20H2, at the same servicing level as the image's base layer, and
**RDP works on it**.

So the experiment was to **swap**: copy the host's working pair over the container's
CU-layer copies.

```powershell
docker stop <container>

# takeown + icacls first: a windowsfilter layer refuses writes on ACL grounds,
# not because it is read-only. Then copy rdpcorets.dll and umb.dll from
# C:\Windows\System32 into the container's layer directory.
takeown /f <layer>\Files\Windows\System32\rdpcorets.dll
icacls  <layer>\Files\Windows\System32\rdpcorets.dll /grant Administrators:F
copy /y C:\Windows\System32\rdpcorets.dll <layer>\Files\Windows\System32\
copy /y C:\Windows\System32\umb.dll       <layer>\Files\Windows\System32\

docker start <container>
```

Result:

```
rdpcorets.dll  1611776   <- byte-identical to the host, unpatched
umb.dll          78848   <- byte-identical to the host
TermService    Running
3389           not listening
```

**The same binaries work on the host and fail in the container. A DLL version
difference is conclusively ruled out.**

> A side note: the static comparison itself was not wrong (`SetupDevice` is
> byte-identical between base and CU, the `CTiBus::Initialize` call sequence is the
> same, `AddBusEnumerator` is structurally the same). **The direction was wrong** —
> the variable was never in the DLLs.

---

## 7. Root cause: the container's object namespace

A **pure Win32** probe — it does not call `umb.dll` and does not use its vtables,
so nothing in it depends on the assumptions under test — was run on the host and in
the container:

| Probe | Host | Container |
|---|---|---|
| `[1]` `SetupDiGetClassDevs` | OK | **OK** |
| `[1]` `CreateFileW` `\\?\root#umbus#0000#{65a9a6cf-…}` | **OK** | **FAILED: 2** |
| `[1]` `CreateFileW` `\\?\umb#umb#1&841921d&0&terminput_bus#{…}` | **OK** | **FAILED: 2** |
| `[2]` `QueryDosDeviceW("root#umbus#0000#{…}")` | **-> `\Device\00000008`** | **NOT IN THIS NAMESPACE: 2** |
| `[2]` `QueryDosDeviceW("umb#umb#1&841921d&0&terminput_bus#{…}")` | **-> `\Device\00000077`** | **NOT IN THIS NAMESPACE: 2** |
| `[3]` `CreateFileW` `\\?\GLOBALROOT\Device\00000008` | **OK** | — |
| `[4]` scan `\Device\00000000`–`\Device\000000FF` (open with no access) | **32 of 256 opened** | **0 of 256 opened** |
| `[5]` **control**: `\\?\GLOBALROOT\Device\KsecDD` / `\Device\Null` | OK | **OK** |

### `[5]` is the control, and it matters

It rules out the false negative that `\\?\GLOBALROOT` paths simply do not resolve
in a container. `KsecDD` and `Null` both open there, **so `[4]`'s `0 of 256` is
real**.

### Reading the table

- **`SetupDiGetClassDevs` succeeds in the container** — the PnP device database
  (`Enum`) is visible, `ROOT\UMBUS\0000` is Started, and the `umbus` driver is
  RUNNING.
- **But not one anonymous device object `\Device\000000NN` can be opened** — they
  do not exist in the container at all.
- **Nor is the interface symbolic link `\??\root#umbus#0000#{…}` in the container's
  namespace.**

**UMBus's device objects are anonymous objects created by PnP enumeration, and they
are not in a container's object namespace.**

So `CreateFileW` returns `ERROR_FILE_NOT_FOUND` — which is that `0x80070002`.

---

## 8. The complete failure chain

```
\Device\000000NN is not in the container's object namespace
  └─ CreateFileW(UMBus interface symlink) -> ERROR_FILE_NOT_FOUND (2) = 0x80070002
       └─ OpenRootUmBusEnumerator fails -> AddBusEnumerator fails
            └─ CTiBus::SetupDevice fails -> CTiBus::Initialize fails
                 └─ CUMRDPProtocolManager::FinalConstruct fails
                      └─ CoCreateInstance(IWRdsProtocolManager) = 0x80070002
                           └─ the listener cannot start
                                └─ (with the bypass patch) session setup hits the
                                   same wall:
                                     CUMTSInputPlugin::CreateInputDevices
                                       └─ CTiDevice::WaitForDeviceReady
                                            └─ WaitForSingleObject(ready event, 20000) == 258
                                                 └─ 0x80070102
                                                      └─ NotifySessionId fails
                                                           └─ AssignSession fails
                                                                └─ client hangs at
                                                                   "configuring the remote session"
                                                                   and disconnects after ~20 s
```

---

## 9. What can and cannot be changed

| | |
|---|---|
| The `TemporaryALiC` registry switch | Yes — Microsoft's own test switch |
| Binary patches to start the listener | Yes — but only by skipping the whole input stack |
| A different image, different DLLs, registry edits | **No.** The limit is in the container's object namespace |
| Making container processes see UMBus device objects | No known way |

**What the bypass costs**: patch 4, skipping `NotifySessionId` entirely, does let a
client connect — but only by **skipping the input stack**, so the result is a black
screen, no input, and a disconnect shortly after.

---

## 10. An unresolved contradiction

The 2018 article's environment (Windows Server 1709) **worked**, and its author
confirmed the host had not changed.

If the limit is at the host kernel / object namespace level, "an image update broke
it" does not explain that. The only self-consistent explanation is that **1709's
`rdpcorets.dll` does not call `CTiBus::Initialize` directly from
`FinalConstruct`**. That was checked:

| | 1709 RTM | 1709+KB4074588 / 20H2 |
|---|---|---|
| `FinalConstruct` calls `CTiBus::Initialize` directly | **No** (only via `CTermInputMgr::Initialize`) | **Yes** |

But the `CTermInputMgr::Initialize` route fails just as hard and passes the same
error through, so **1709 reaching session setup would hit the same wall in
`CreateInputDevices`**.

**Closing this needs a 1709 host** (the author's is gone). Without one it stays
open.

---

## Appendix A: key functions and offsets

All in `rdpcorets.dll` 10.0.19041.x from the `servercore:20H2` image.

| Function | |
|---|---|
| `CUMRDPProtocolManager::FinalConstruct` | Calls `CTiBus::Initialize` during listener creation |
| `CUMRDPConnection::NotifySessionId` | Session setup; the input block and the graphics block |
| `CUMTSInputPlugin::CreateInputDevices` | Creates the input devices |
| `CTiDevice::WaitForDeviceReady` | Waits 20 s, returns `0x80070102` on timeout |
| `CTiDevice::Initialize` | `CreateEventW` creates the ready event |
| `CTiDevice::SignalReady` | Only signalled on `SetupDevice`'s success path |
| `CTiBus::Initialize` / `SetupDevice` | Four failure points; `SetupDevice` calls `AddBusEnumerator` |
| `umb.dll!CUMBusDriver::AddBusEnumerator` | Vtable slot 6 (offset 48) |
| `umb.dll!CUMBusDriver::CreateBusEnumerator` | `DeviceIoControl(hUMBus, 0x228000, <DWORD 1><name>, ...)` |

### Experimental patches (**not fixes**; only to keep analysing)

| Offset | Original | Replacement | Effect |
|---|---|---|---|
| `0x16617` | `8b f0` | `31 f6` | `FinalConstruct` ignores `CTiBus::Initialize`'s failure |
| `0x16619` | `85 c0` | `31 c0` | As above |
| `0x464b0` | `48 89 5c` | `31 c0 c3` | Skip `NotifySessionId` entirely (crudest) |
| `0x4711d` | `8b c7` | `31 c0` | Run `NotifySessionId` but return 0 |
| `0x46a53` | `0f 85 49 05 00 00` | `e9 4a 05 00 00 90` | Do the input init, skip the graphics block |

`file_offset = VA - 0x180000C00` for this image version.

### WPP providers that are useful and easy to get wrong

| GUID | Component |
|---|---|
| `{A3BDED7C-8BEE-3E5A-6499-D41A36E139A4}` | `FinalConstruct` / `CTermInputMgr::Initialize` |
| `{01F19864-60E9-3C8F-C234-B92505F16052}` | `CTiBus::Initialize` / `CPNPPropertyStore` |
| `{36ED36ED-D378-351D-064C-923CC188FD7C}` | `CTiBus::Initialize` / `CTiDevice` |
| `{FD3EE561-44F4-339B-A20A-AFC708B1F8D0}` | `CUMTSInputPlugin::Add*Device` |
| `{68A65793-594D-393D-5222-0CCF4D4C2E51}` | `CUMRDPListenerVMBus` — **unrelated to this failure** |

## Appendix B: what the investigation needed

These were written for this analysis and are **not part of this repository**. They
are described so the work can be redone, not so they can be run from here.

| Tool | Purpose |
|---|---|
| A pure Win32 UMBus probe | **The decisive one.** It does not call `umb.dll` and does not use its vtables, so nothing in it depends on the assumptions under test. Run on the host and in the container and compare the two outputs. Statically linked (`/MT`) so it needs no runtime in the container |
| A PnP state dump | Inside the container: `Get-PnpDevice`, the relevant registry keys, `pnputil` as a fallback |
| A layer swap script | The swap experiment in section 6: copies host files into a container layer, with an undo path |
| A WPP capture script | Starts an ETW session against termsrv, lsm and rdpcorets and collects the trace |
| A host-side patcher | Applies the offsets in appendix A to a container layer, with an undo path |

## Appendix C: pitfalls

1. **The wrong WPP provider GUID** — zero events captured, a round wasted. See the
   encoding note in section 4.
2. **`logman`'s level is a UCHAR** — `0xFFFFFFFF` reports "Out of present range.";
   write `255`.
3. **`CM_PROB_PHANTOM` is not a fault.** The `TERMINPUT_BUS` enumerator is created
   **during session setup**, so it is phantom until an RDP client connects and OK
   afterwards. Reading it as a fault sends you the wrong way.
4. **Calling `umb.dll`'s COM objects from an external process crashes easily** —
   assumptions about vtable slots and calling conventions go wrong quietly. **If
   Win32 system calls can settle a question, do not call internal functions.**
5. **Writing to a `windowsfilter` layer is an ACL problem, not a read-only
   attribute** — it needs `takeown` and `icacls` first.
6. **Process-isolated containers share the host kernel**, so "driver state in the
   container" and "driver state on the host" can differ, and the `.sys` files in
   the image are never loaded at all.

---

*This records analysis of a public image and public system components on a private
virtual machine.*
