# SecureComms — Windows Kernel Driver Template

A production-shaped **WDM** sample driver demonstrating a secure user↔kernel
communication channel and process-creation monitoring. It is written as a
teaching/starter template: every user buffer is validated, every allocation is
tagged, synchronization is explicit, and the load/unload paths are race-free.

> **Scope & intent.** This is a *defensive/observability* template. It monitors
> process creation the same way an EDR sensor or audit tool does, using the
> documented `PsSetCreateProcessNotifyRoutineEx` API. It contains no hiding,
> tampering, or evasion behavior.

## Layout

| Path | Purpose |
|------|---------|
| `inc/SecureComms_Public.h` | Shared ABI: device names, IOCTL codes, request/response structs (included by driver **and** client). |
| `src/Driver.h`             | Kernel-only internal declarations (device extension, ring buffer, prototypes). |
| `src/Driver.c`             | Driver implementation: `DriverEntry`, dispatch, IOCTL handlers, notify callback, unload. |
| `src/SecureComms.inf`      | Install INF for a demand-start kernel service. |
| `src/SecureComms.vcxproj`  | VS + WDK project file. |
| `client/Client.c`          | User-mode demonstration client. |

## Architecture

### 1. Secure device object & symbolic link
`DriverEntry` calls **`IoCreateDeviceSecure`** with an explicit SDDL string:

```
D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)
```

— full access to SYSTEM and Administrators, read-only to authenticated users.
`FILE_DEVICE_SECURE_OPEN` extends the check to opens with a trailing path. A
symbolic link (`\DosDevices\SecureComms`) exposes the device to Win32 as
`\\.\SecureComms`. `DriverUnload` tears everything down in the correct order.

### 2. IOCTL dispatcher (`METHOD_BUFFERED`)
Handlers for `IRP_MJ_CREATE`, `IRP_MJ_CLOSE`, and `IRP_MJ_DEVICE_CONTROL`.
Each IOCTL validates `InputBufferLength` / `OutputBufferLength` against the
declared structure sizes with **overflow-safe** arithmetic before touching a
byte, checks a protocol-version field, and completes the IRP with an accurate
`IoStatus.Information`. Control codes that mutate state require
`FILE_WRITE_ACCESS`.

| IOCTL | Access | Meaning |
|-------|--------|---------|
| `GET_VERSION`     | read  | protocol version + counters |
| `ECHO`            | any   | length-checked round-trip |
| `SET_MONITORING`  | write | enable/disable the notify callback's effect |
| `DRAIN_EVENTS`    | read  | copy buffered process events to caller |

### 3. Process-creation callback
Registered once via `PsSetCreateProcessNotifyRoutineEx`. It reads the enable
flag without blocking, copies the image name with `RtlStringCchCopyNW`
(truncation-safe on a counted, possibly non-terminated source), and enqueues an
event under a spinlock. It is **not** paged and makes no blocking calls, so it
is correct at both `PASSIVE_LEVEL` and `APC_LEVEL`. Unregistration in
`DriverUnload` (with `Remove == TRUE`) guarantees no callback is running before
the device extension is freed.

### 4. Memory & synchronization
* `#pragma alloc_text` marks `DriverEntry` `INIT` and the PASSIVE-only handlers
  `PAGE`; the callback and ring producer stay resident.
* **`KSPIN_LOCK`** guards the hot-path event ring (producer = callback,
  consumer = drain IOCTL).
* **`FAST_MUTEX`** guards cold-path configuration state (PASSIVE only).
* Counters use interlocked ops.
* All copies use bounded primitives (`RtlMoveMemory` for the overlap-capable
  buffered-IOCTL echo, `RtlStringCchCopyNW` for strings) with explicit null and
  length checks.

## Building

Install **Visual Studio 2022** + the matching **WDK** and Spectre-mitigated
libraries, then either open `src/SecureComms.vcxproj` or run from a Developer
Command Prompt:

```
msbuild src\SecureComms.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output: `SecureComms.sys` (+ `.inf`, and a `.cat` if you stamp/sign it).

## Installing & testing (test machine only)

Kernel drivers must be signed for production. On a **dedicated test VM**, enable
test signing and load the demand-start service:

```
bcdedit /set testsigning on          :: then reboot
sc create SecureComms type= kernel binPath= C:\path\SecureComms.sys
sc start  SecureComms
Client.exe 10                         :: run elevated
sc stop   SecureComms
sc delete SecureComms
```

Use a VM with a kernel debugger attached; a bug here bugchecks the machine.

## Notes / hardening ideas

* Add a per-handle `FILE_OBJECT` context if you need per-client event queues.
* Consider `PsSetCreateProcessNotifyRoutineEx2` for richer info on modern OSes.
* For high event rates, replace the fixed ring with a lookaside-backed queue and
  signal an event so clients can wait instead of poll.
