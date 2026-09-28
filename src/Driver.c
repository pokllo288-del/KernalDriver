/*++

Module Name:

    Driver.c

Abstract:

    A production-shaped WDM sample driver ("SecureComms") demonstrating:

      1. A secure named device object created with an explicit SDDL security
         descriptor, plus a symbolic link for user-mode access, and a clean
         teardown path in DriverUnload.

      2. An IOCTL dispatcher (CREATE / CLOSE / DEVICE_CONTROL) using
         METHOD_BUFFERED with rigorous input/output length validation and
         overflow-safe arithmetic.

      3. Process-creation monitoring via PsSetCreateProcessNotifyRoutineEx,
         with the events buffered into a lock-protected ring and drained on
         demand by a client.

      4. Correct synchronization: a KSPIN_LOCK for the hot-path ring buffer
         (touched by the notify callback) and a FAST_MUTEX for cold-path
         configuration state, plus pageable-section directives.

    The driver is intentionally conservative: every user buffer is validated,
    every allocation is tagged, and the unload path is idempotent.

Environment:

    Kernel mode.

--*/

#include "Driver.h"

//
// Tell the linker which routines may live in the pageable "PAGE" section.
// These run only at PASSIVE_LEVEL. The notify callback and the ring-buffer
// producer are deliberately NOT paged, because the callback can be invoked at
// raised IRQL and while holding a spinlock we must not touch paged memory.
//
#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, SecureCommsUnload)
#pragma alloc_text(PAGE, SecureCommsCreateClose)
#pragma alloc_text(PAGE, SecureCommsDeviceControl)
#endif

//
// Global device-object back-pointer (declared extern in Driver.h). The process
// notify callback has no context argument, so it reaches driver state through
// this pointer.
//
PDEVICE_OBJECT g_SecureCommsDeviceObject = NULL;

//
// Security descriptor for the device object, in SDDL form.
//
//   D:P                       DACL, protected (no inherited ACEs)
//   (A;;GA;;;SY)              Allow  GENERIC_ALL  to Local System
//   (A;;GA;;;BA)              Allow  GENERIC_ALL  to Built-in Administrators
//   (A;;GR;;;AU)              Allow  GENERIC_READ to Authenticated Users
//
// The result: only SYSTEM and Administrators may open the device for the
// state-changing IOCTLs (which demand FILE_WRITE_ACCESS); ordinary
// authenticated users get read-only reach at most. Adjust to taste; the point
// is that access is explicit rather than inherited from a default.
//
DECLARE_CONST_UNICODE_STRING(
    g_SecureCommsSddl,
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)");

//
// A device class GUID is required by IoCreateDeviceSecure. This is an
// arbitrary, driver-private GUID (generated for this sample). It has nothing to
// do with setup classes; it simply names the security metadata.
//
// {7B2E4C10-9A3D-4E2A-8F1B-6C5D4E3A2B1C}
//
static const GUID GUID_SECURECOMMS_DEVCLASS =
    { 0x7b2e4c10, 0x9a3d, 0x4e2a,
      { 0x8f, 0x1b, 0x6c, 0x5d, 0x4e, 0x3a, 0x2b, 0x1c } };


//
// ===========================================================================
//  Ring buffer helpers (producer/consumer)
// ===========================================================================
//

//
// Push one event into the ring. Producer side, called from the notify
// callback. Runs with EventLock held. Non-pageable by virtue of being called
// under a spinlock; keep it small and self-contained.
//
_Requires_lock_held_(devExt->EventLock)
static VOID
SecureCommsRingPush(
    _Inout_ PSECURECOMMS_DEVICE_EXTENSION devExt,
    _In_ const SECURECOMMS_PROCESS_EVENT* event
    )
{
    if (devExt->EventCount >= SECURECOMMS_EVENT_RING_SIZE) {
        //
        // Ring is full: drop the newest event rather than overwrite an
        // undrained one, and account for the loss. Interlocked because the
        // counter is also read (unlocked) by the version IOCTL.
        //
        InterlockedIncrement64(&devExt->EventsDropped);
        return;
    }

    devExt->EventRing[devExt->EventHead] = *event;
    devExt->EventHead = (devExt->EventHead + 1) % SECURECOMMS_EVENT_RING_SIZE;
    devExt->EventCount++;
}

//
// Pop one event from the ring. Consumer side, called from the drain IOCTL.
// Runs with EventLock held. Returns FALSE when the ring is empty.
//
_Requires_lock_held_(devExt->EventLock)
static BOOLEAN
SecureCommsRingPop(
    _Inout_ PSECURECOMMS_DEVICE_EXTENSION devExt,
    _Out_ SECURECOMMS_PROCESS_EVENT* event
    )
{
    if (devExt->EventCount == 0) {
        RtlZeroMemory(event, sizeof(*event));
        return FALSE;
    }

    *event = devExt->EventRing[devExt->EventTail];
    devExt->EventTail = (devExt->EventTail + 1) % SECURECOMMS_EVENT_RING_SIZE;
    devExt->EventCount--;
    return TRUE;
}


//
// ===========================================================================
//  Process-creation notify callback
// ===========================================================================
//
// Contract (per WDK): PsSetCreateProcessNotifyRoutineEx callbacks are invoked
// at PASSIVE_LEVEL for creation and can be at APC_LEVEL for some paths; we make
// no blocking calls and touch only non-paged state under a spinlock, so we are
// safe either way. The callback must be brief and must not call back into the
// process manager in a way that re-enters us.
//
VOID
SecureCommsCreateProcessNotifyEx(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    PSECURECOMMS_DEVICE_EXTENSION devExt;
    SECURECOMMS_PROCESS_EVENT event;
    KIRQL oldIrql;
    BOOLEAN monitoring;

    UNREFERENCED_PARAMETER(Process);

    if (g_SecureCommsDeviceObject == NULL) {
        return;
    }
    devExt = (PSECURECOMMS_DEVICE_EXTENSION)
                 g_SecureCommsDeviceObject->DeviceExtension;

    //
    // Read the monitoring flag without taking the fast mutex: the callback may
    // run at APC_LEVEL where a fast mutex is illegal. A plain read of a BOOLEAN
    // is atomic on all supported architectures; a stale value merely means we
    // buffer one extra (or one fewer) event around a toggle, which is benign.
    //
    monitoring = devExt->MonitoringEnabled;
    if (!monitoring) {
        return;
    }

    RtlZeroMemory(&event, sizeof(event));
    event.ProcessId = (unsigned long long)(ULONG_PTR)ProcessId;

    if (CreateInfo != NULL) {
        //
        // Process creation.
        //
        event.IsCreate = 1;
        event.ParentProcessId =
            (unsigned long long)(ULONG_PTR)CreateInfo->ParentProcessId;
        event.CreatingThreadProcessId =
            (unsigned long long)(ULONG_PTR)
                CreateInfo->CreatingThreadId.UniqueProcess;

        //
        // Copy the image name defensively. ImageFileName is a counted
        // UNICODE_STRING owned by the OS; it may be NULL. We copy at most
        // (SECURECOMMS_IMAGE_NAME_CCH - 1) characters and always NUL-terminate.
        // RtlStringCchCopyNW handles truncation without overflow.
        //
        if (CreateInfo->ImageFileName != NULL &&
            CreateInfo->ImageFileName->Buffer != NULL &&
            CreateInfo->ImageFileName->Length > 0) {

            SIZE_T srcCch = CreateInfo->ImageFileName->Length / sizeof(WCHAR);
            SIZE_T copyCch = min(srcCch, (SIZE_T)(SECURECOMMS_IMAGE_NAME_CCH - 1));

            //
            // RtlStringCchCopyNW copies at most copyCch chars from a source
            // that is NOT required to be NUL-terminated, then terminates the
            // destination. This is the correct primitive for a counted string.
            //
            if (NT_SUCCESS(RtlStringCchCopyNW(event.ImageName,
                                              SECURECOMMS_IMAGE_NAME_CCH,
                                              CreateInfo->ImageFileName->Buffer,
                                              copyCch))) {
                event.ImageNameLengthCch = (unsigned long)copyCch;
            }
        }
    } else {
        //
        // Process exit: CreateInfo is NULL.
        //
        event.IsCreate = 0;
    }

    //
    // Enqueue under the spinlock. KeAcquireSpinLock raises to DISPATCH_LEVEL;
    // everything we touch here is non-paged, so this is safe.
    //
    KeAcquireSpinLock(&devExt->EventLock, &oldIrql);
    SecureCommsRingPush(devExt, &event);
    KeReleaseSpinLock(&devExt->EventLock, oldIrql);
}


//
// ===========================================================================
//  CREATE / CLOSE dispatch
// ===========================================================================
//
NTSTATUS
SecureCommsCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION irpSp;
    PSECURECOMMS_DEVICE_EXTENSION devExt;

    PAGED_CODE();

    devExt = (PSECURECOMMS_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    irpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (irpSp->MajorFunction) {
    case IRP_MJ_CREATE:
        InterlockedIncrement(&devExt->OpenHandleCount);
        break;
    case IRP_MJ_CLOSE:
        InterlockedDecrement(&devExt->OpenHandleCount);
        break;
    default:
        break;
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}


//
// ===========================================================================
//  DEVICE_CONTROL dispatch and per-IOCTL handlers
// ===========================================================================
//

//
// IOCTL_SECURECOMMS_GET_VERSION handler.
// Input: none. Output: SECURECOMMS_VERSION_INFO.
//
static NTSTATUS
SecureCommsHandleGetVersion(
    _In_ PSECURECOMMS_DEVICE_EXTENSION devExt,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION irpSp,
    _Out_ PULONG_PTR BytesReturned
    )
{
    PSECURECOMMS_VERSION_INFO info;
    KIRQL oldIrql;

    *BytesReturned = 0;

    if (irpSp->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(SECURECOMMS_VERSION_INFO)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    //
    // METHOD_BUFFERED: input and output share Irp->AssociatedIrp.SystemBuffer.
    //
    info = (PSECURECOMMS_VERSION_INFO)Irp->AssociatedIrp.SystemBuffer;
    RtlZeroMemory(info, sizeof(*info));

    info->ProtocolVersion = SECURECOMMS_PROTOCOL_VERSION;

    ExAcquireFastMutex(&devExt->StateLock);
    info->MonitoringEnabled = devExt->MonitoringEnabled ? 1u : 0u;
    ExReleaseFastMutex(&devExt->StateLock);

    KeAcquireSpinLock(&devExt->EventLock, &oldIrql);
    info->EventsBuffered = devExt->EventCount;
    KeReleaseSpinLock(&devExt->EventLock, oldIrql);

    info->EventsDropped =
        (unsigned long)InterlockedCompareExchange64(&devExt->EventsDropped, 0, 0);

    *BytesReturned = sizeof(SECURECOMMS_VERSION_INFO);
    return STATUS_SUCCESS;
}

//
// IOCTL_SECURECOMMS_ECHO handler.
// Input: SECURECOMMS_ECHO_REQUEST (+PayloadLength bytes). Output: payload bytes.
//
static NTSTATUS
SecureCommsHandleEcho(
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION irpSp,
    _Out_ PULONG_PTR BytesReturned
    )
{
    PSECURECOMMS_ECHO_REQUEST req;
    ULONG inLen  = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG payloadLen;
    PVOID systemBuffer = Irp->AssociatedIrp.SystemBuffer;

    *BytesReturned = 0;

    //
    // Must at least contain the fixed header (version + length). The flexible
    // array member Payload[1] means sizeof() already includes one byte; we
    // validate against the header offset explicitly to avoid off-by-one traps.
    //
    if (inLen < FIELD_OFFSET(SECURECOMMS_ECHO_REQUEST, Payload)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    req = (PSECURECOMMS_ECHO_REQUEST)systemBuffer;

    if (req->ProtocolVersion != SECURECOMMS_PROTOCOL_VERSION) {
        return STATUS_REVISION_MISMATCH;
    }

    payloadLen = req->PayloadLength;

    //
    // Bound the caller-supplied length before doing any arithmetic with it.
    //
    if (payloadLen > SECURECOMMS_MAX_PAYLOAD) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    //
    // Overflow-safe check that the input buffer actually holds the declared
    // payload. FIELD_OFFSET + payloadLen cannot overflow a ULONG here because
    // payloadLen <= SECURECOMMS_MAX_PAYLOAD (4096) and the offset is tiny, but
    // we still compute the bound so the relationship is explicit and auditable.
    //
    if (payloadLen > inLen - FIELD_OFFSET(SECURECOMMS_ECHO_REQUEST, Payload)) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    //
    // Output must be able to hold the echoed payload.
    //
    if (outLen < payloadLen) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    //
    // METHOD_BUFFERED aliases input and output onto the same system buffer.
    // Copy forward within the buffer; source and destination can overlap when
    // the payload starts mid-buffer, so use RtlMoveMemory (memmove semantics),
    // not RtlCopyMemory.
    //
    if (payloadLen > 0) {
        RtlMoveMemory(systemBuffer, req->Payload, payloadLen);
    }

    *BytesReturned = payloadLen;
    return STATUS_SUCCESS;
}

//
// IOCTL_SECURECOMMS_SET_MONITORING handler.
// Input: SECURECOMMS_SET_MONITORING_REQUEST. Output: none.
//
static NTSTATUS
SecureCommsHandleSetMonitoring(
    _In_ PSECURECOMMS_DEVICE_EXTENSION devExt,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION irpSp
    )
{
    PSECURECOMMS_SET_MONITORING_REQUEST req;

    if (irpSp->Parameters.DeviceIoControl.InputBufferLength <
            sizeof(SECURECOMMS_SET_MONITORING_REQUEST)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    req = (PSECURECOMMS_SET_MONITORING_REQUEST)Irp->AssociatedIrp.SystemBuffer;

    if (req->ProtocolVersion != SECURECOMMS_PROTOCOL_VERSION) {
        return STATUS_REVISION_MISMATCH;
    }

    //
    // The OS callback itself is registered once at DriverEntry and stays
    // registered for the driver's lifetime; this IOCTL only flips a flag that
    // the callback consults. That avoids repeated register/unregister churn
    // (which has its own synchronization hazards) on every toggle.
    //
    ExAcquireFastMutex(&devExt->StateLock);
    devExt->MonitoringEnabled = (req->Enable != 0) ? TRUE : FALSE;
    ExReleaseFastMutex(&devExt->StateLock);

    return STATUS_SUCCESS;
}

//
// IOCTL_SECURECOMMS_DRAIN_EVENTS handler.
// Input: none. Output: SECURECOMMS_DRAIN_EVENTS_RESULT (+events).
//
static NTSTATUS
SecureCommsHandleDrainEvents(
    _In_ PSECURECOMMS_DEVICE_EXTENSION devExt,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION irpSp,
    _Out_ PULONG_PTR BytesReturned
    )
{
    PSECURECOMMS_DRAIN_EVENTS_RESULT result;
    ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG headerLen = FIELD_OFFSET(SECURECOMMS_DRAIN_EVENTS_RESULT, Events);
    ULONG capacityEvents;
    ULONG produced = 0;
    KIRQL oldIrql;

    *BytesReturned = 0;

    if (outLen < headerLen) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    //
    // How many whole events fit in the caller's buffer after the header?
    // Integer division cannot overflow and yields a safe upper bound.
    //
    capacityEvents = (outLen - headerLen) / sizeof(SECURECOMMS_PROCESS_EVENT);
    if (capacityEvents > SECURECOMMS_MAX_EVENTS) {
        capacityEvents = SECURECOMMS_MAX_EVENTS;
    }

    result = (PSECURECOMMS_DRAIN_EVENTS_RESULT)Irp->AssociatedIrp.SystemBuffer;

    //
    // Drain under the spinlock. We copy directly into the (fixed, non-paged
    // during this call) system buffer, so touching it under the lock is fine.
    //
    KeAcquireSpinLock(&devExt->EventLock, &oldIrql);

    while (produced < capacityEvents) {
        SECURECOMMS_PROCESS_EVENT ev;
        if (!SecureCommsRingPop(devExt, &ev)) {
            break;
        }
        result->Events[produced] = ev;
        produced++;
    }

    result->EventCount = produced;
    result->Remaining = devExt->EventCount;

    KeReleaseSpinLock(&devExt->EventLock, oldIrql);

    //
    // Report exactly the bytes written: header + the events we produced.
    //
    *BytesReturned = headerLen +
                     (ULONG_PTR)produced * sizeof(SECURECOMMS_PROCESS_EVENT);
    return STATUS_SUCCESS;
}

//
// Top-level DEVICE_CONTROL dispatch. Validates the control code, routes to the
// handler, and completes the IRP with an accurate Information (bytes returned)
// value in all paths.
//
NTSTATUS
SecureCommsDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION irpSp;
    PSECURECOMMS_DEVICE_EXTENSION devExt;
    NTSTATUS status;
    ULONG_PTR bytesReturned = 0;
    ULONG controlCode;

    PAGED_CODE();

    devExt = (PSECURECOMMS_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    irpSp = IoGetCurrentIrpStackLocation(Irp);
    controlCode = irpSp->Parameters.DeviceIoControl.IoControlCode;

    switch (controlCode) {
    case IOCTL_SECURECOMMS_GET_VERSION:
        status = SecureCommsHandleGetVersion(devExt, Irp, irpSp, &bytesReturned);
        break;

    case IOCTL_SECURECOMMS_ECHO:
        status = SecureCommsHandleEcho(Irp, irpSp, &bytesReturned);
        break;

    case IOCTL_SECURECOMMS_SET_MONITORING:
        status = SecureCommsHandleSetMonitoring(devExt, Irp, irpSp);
        break;

    case IOCTL_SECURECOMMS_DRAIN_EVENTS:
        status = SecureCommsHandleDrainEvents(devExt, Irp, irpSp, &bytesReturned);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytesReturned;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}


//
// ===========================================================================
//  Unload
// ===========================================================================
//
VOID
SecureCommsUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    PDEVICE_OBJECT deviceObject = DriverObject->DeviceObject;
    PSECURECOMMS_DEVICE_EXTENSION devExt;
    UNICODE_STRING symlinkName;

    PAGED_CODE();

    //
    // Unregister the process notify callback FIRST, before tearing down the
    // device. PsSetCreateProcessNotifyRoutineEx with Remove==TRUE guarantees
    // that no callback is running and none will start once it returns, so the
    // device extension can then be freed safely. This is the critical ordering
    // for a race-free unload.
    //
    if (deviceObject != NULL) {
        devExt = (PSECURECOMMS_DEVICE_EXTENSION)deviceObject->DeviceExtension;

        if (devExt != NULL && devExt->CallbackRegistered) {
            (VOID)PsSetCreateProcessNotifyRoutineEx(
                      SecureCommsCreateProcessNotifyEx,
                      TRUE /* Remove */);
            devExt->CallbackRegistered = FALSE;
        }
    }

    //
    // Now the callback is quiesced; clear the global pointer so any stale
    // reference (there should be none) sees NULL.
    //
    g_SecureCommsDeviceObject = NULL;

    //
    // Delete the symbolic link, then the device object. Order matters: remove
    // the user-visible name first so no new opens can arrive against a device
    // we are about to delete.
    //
    RtlInitUnicodeString(&symlinkName, SECURECOMMS_SYMLINK_NAME);
    (VOID)IoDeleteSymbolicLink(&symlinkName);

    if (deviceObject != NULL) {
        IoDeleteDevice(deviceObject);
    }
}


//
// ===========================================================================
//  DriverEntry
// ===========================================================================
//
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    PDEVICE_OBJECT deviceObject = NULL;
    PSECURECOMMS_DEVICE_EXTENSION devExt;
    UNICODE_STRING deviceName;
    UNICODE_STRING symlinkName;
    BOOLEAN symlinkCreated = FALSE;
    BOOLEAN callbackRegistered = FALSE;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&deviceName, SECURECOMMS_DEVICE_NAME);
    RtlInitUnicodeString(&symlinkName, SECURECOMMS_SYMLINK_NAME);

    //
    // Create the device object with an explicit security descriptor.
    // IoCreateDeviceSecure applies the SDDL DACL to the device's object header,
    // so the reference monitor enforces our access policy on every open. We
    // request FILE_DEVICE_SECURE_OPEN so the same check also applies to opens
    // that specify a trailing path component.
    //
    status = IoCreateDeviceSecure(
                 DriverObject,
                 sizeof(SECURECOMMS_DEVICE_EXTENSION),
                 &deviceName,
                 FILE_DEVICE_UNKNOWN,
                 FILE_DEVICE_SECURE_OPEN,
                 FALSE,                       // not exclusive
                 &g_SecureCommsSddl,          // SDDL security descriptor
                 (LPCGUID)&GUID_SECURECOMMS_DEVCLASS,
                 &deviceObject);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Initialize the device extension (zeroed by IoCreateDeviceSecure).
    //
    devExt = (PSECURECOMMS_DEVICE_EXTENSION)deviceObject->DeviceExtension;
    devExt->DeviceObject = deviceObject;
    devExt->MonitoringEnabled = FALSE;
    devExt->CallbackRegistered = FALSE;
    devExt->EventHead = 0;
    devExt->EventTail = 0;
    devExt->EventCount = 0;
    devExt->EventsDropped = 0;
    devExt->OpenHandleCount = 0;

    ExInitializeFastMutex(&devExt->StateLock);
    KeInitializeSpinLock(&devExt->EventLock);

    //
    // We use buffered I/O for our control buffers (METHOD_BUFFERED IOCTLs).
    // Setting DO_BUFFERED_IO is not strictly required for DeviceIoControl
    // (the method bits in each control code govern that), but it is the correct
    // default for a device that also handles read/write via buffered I/O.
    //
    deviceObject->Flags |= DO_BUFFERED_IO;

    //
    // Publish the global pointer before registering the callback: the callback
    // may fire immediately upon registration and dereferences this pointer.
    //
    g_SecureCommsDeviceObject = deviceObject;

    //
    // Wire up dispatch routines and the unload routine.
    //
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = SecureCommsCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = SecureCommsCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SecureCommsDeviceControl;
    DriverObject->DriverUnload                         = SecureCommsUnload;

    //
    // Create the symbolic link so user mode can reach the device by
    // "\\.\SecureComms".
    //
    status = IoCreateSymbolicLink(&symlinkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        goto Fail;
    }
    symlinkCreated = TRUE;

    //
    // Register the process-creation notify callback. Monitoring starts DISABLED
    // (the flag is FALSE); the callback returns early until a client enables it
    // via IOCTL_SECURECOMMS_SET_MONITORING. Registering up front avoids
    // register/unregister races on every toggle.
    //
    status = PsSetCreateProcessNotifyRoutineEx(
                 SecureCommsCreateProcessNotifyEx,
                 FALSE /* Remove == FALSE => register */);
    if (!NT_SUCCESS(status)) {
        //
        // A common failure here is STATUS_ACCESS_DENIED when the driver image
        // is not signed with the required attributes on a production system.
        //
        goto Fail;
    }
    callbackRegistered = TRUE;
    devExt->CallbackRegistered = TRUE;

    return STATUS_SUCCESS;

Fail:

    //
    // Unwind in reverse order of acquisition. Each step is guarded so the path
    // is correct regardless of how far initialization progressed.
    //
    if (callbackRegistered) {
        (VOID)PsSetCreateProcessNotifyRoutineEx(
                  SecureCommsCreateProcessNotifyEx, TRUE);
    }
    if (symlinkCreated) {
        (VOID)IoDeleteSymbolicLink(&symlinkName);
    }

    g_SecureCommsDeviceObject = NULL;

    if (deviceObject != NULL) {
        IoDeleteDevice(deviceObject);
    }

    return status;
}
