/*++

Module Name:

    Driver.h

Abstract:

    Internal (kernel-only) declarations for the SecureComms driver: the global
    device context, the process-event ring buffer, and the routine prototypes
    shared across the driver's source files.

Environment:

    Kernel mode only.

--*/

#ifndef _SECURECOMMS_DRIVER_H_
#define _SECURECOMMS_DRIVER_H_

#include <ntddk.h>
#include <wdmsec.h>     // IoCreateDeviceSecure, SDDL_* device class GUIDs
#include <ntstrsafe.h>  // RtlStringCch* safe string helpers

#include "../inc/SecureComms_Public.h"

//
// Pool tag ('mCoS' shows as "SoCm" in tools like poolmon). Used for every
// allocation so leaks are attributable to this driver.
//
#define SECURECOMMS_POOL_TAG    'mCoS'

//
// Ring-buffer capacity for buffered process events. A power of two keeps the
// index math simple; SECURECOMMS_MAX_EVENTS is the public drain cap.
//
#define SECURECOMMS_EVENT_RING_SIZE   256

//
// Device extension: the driver's shared, mutable state. Access to the mutable
// fields is serialized either by EventLock (the ring buffer, hot path, touched
// from the notify callback at <= APC_LEVEL) or StateLock (configuration, cold
// path, touched only at PASSIVE_LEVEL from IOCTL handlers).
//
typedef struct _SECURECOMMS_DEVICE_EXTENSION {

    PDEVICE_OBJECT      DeviceObject;

    //
    // Guards MonitoringEnabled and the CallbackRegistered flag. A fast mutex
    // is appropriate here: these are cold-path, PASSIVE_LEVEL-only fields and
    // a fast mutex may block, which is fine outside the notify callback.
    //
    FAST_MUTEX          StateLock;
    BOOLEAN             MonitoringEnabled;
    BOOLEAN             CallbackRegistered;

    //
    // Guards the event ring buffer. A spinlock is required because the process
    // notify callback can run at IRQL up to APC_LEVEL and must not block; the
    // ring is a producer (callback) / consumer (drain IOCTL) queue.
    //
    KSPIN_LOCK          EventLock;
    SECURECOMMS_PROCESS_EVENT   EventRing[SECURECOMMS_EVENT_RING_SIZE];
    ULONG               EventHead;      // next slot to write (producer)
    ULONG               EventTail;      // next slot to read  (consumer)
    ULONG               EventCount;     // items currently in the ring
    volatile LONG64     EventsDropped;  // total events dropped (ring full)

    //
    // Count of currently open handles. Purely informational / diagnostic;
    // updated with interlocked ops so it needs no lock.
    //
    volatile LONG       OpenHandleCount;

} SECURECOMMS_DEVICE_EXTENSION, *PSECURECOMMS_DEVICE_EXTENSION;

//
// Single global back-pointer to the device object, needed by the process
// notify callback (which receives no context parameter). Set once in
// DriverEntry, cleared in DriverUnload, and only read after the callback is
// registered / before it is unregistered, so it needs no separate lock.
//
extern PDEVICE_OBJECT g_SecureCommsDeviceObject;

//
// ---- Routine prototypes ---------------------------------------------------
//

DRIVER_INITIALIZE   DriverEntry;
DRIVER_UNLOAD       SecureCommsUnload;

_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
DRIVER_DISPATCH     SecureCommsCreateClose;

_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH     SecureCommsDeviceControl;

//
// Process-creation notify callback (Ex variant).
//
VOID
SecureCommsCreateProcessNotifyEx(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    );

#endif // _SECURECOMMS_DRIVER_H_
