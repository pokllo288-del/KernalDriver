/*++

    FOR EDUCATIONAL PURPOSES ONLY. Reference/learning template; not for
    production use. Test only on systems you own or are authorized to test.

Module Name:

    SecureComms_Public.h

Abstract:

    Shared public interface definitions for the SecureComms sample driver.

    This header is included by BOTH the kernel-mode driver and any user-mode
    client. It contains the device name, the symbolic link name, the IOCTL
    control codes, and the request/response structures that flow across the
    user/kernel boundary. Keeping these in a single header guarantees that
    both sides agree on the ABI (structure layout, control codes, buffer
    sizes).

Environment:

    Kernel mode and user mode.

--*/

#ifndef _SECURECOMMS_PUBLIC_H_
#define _SECURECOMMS_PUBLIC_H_

//
// Device and symbolic link names.
//
// The device is created in the \Device object namespace (kernel visible only).
// The symbolic link is created in \DosDevices (a.k.a. \??), which is what
// user mode reaches through the "\\.\SecureComms" Win32 path.
//
#define SECURECOMMS_DEVICE_NAME     L"\\Device\\SecureComms"
#define SECURECOMMS_SYMLINK_NAME    L"\\DosDevices\\SecureComms"
#define SECURECOMMS_WIN32_NAME      L"\\\\.\\SecureComms"

//
// Device type for our IOCTLs. 0x8000-0xFFFF is reserved for vendors, per the
// CTL_CODE contract in devioctl.h / winioctl.h.
//
#define SECURECOMMS_DEVICE_TYPE     0x8000

//
// Protocol versioning. The client sends its version in every request so the
// driver can reject mismatched clients cleanly instead of misinterpreting
// a buffer laid out by an incompatible build.
//
#define SECURECOMMS_PROTOCOL_VERSION    0x00010000UL  // 1.0

//
// Bounds used for buffer validation on both sides. Kept small and explicit so
// overflow / arithmetic-overflow checks are trivial to reason about.
//
#define SECURECOMMS_MAX_PAYLOAD     4096
#define SECURECOMMS_MAX_EVENTS      64
#define SECURECOMMS_IMAGE_NAME_CCH  260   // MAX_PATH in characters

//
// IOCTL control codes.
//
// METHOD_BUFFERED: the I/O manager allocates a single system buffer, copies
// the caller's input into it on the way in, and copies the driver's output
// back out on the way out. This is the safest method because the driver never
// touches a raw user-mode pointer for the control buffers.
//
// FILE_ANY_ACCESS is combined with an ACL on the device object (see the SDDL
// string in the driver) to gate who may open the device at all. For codes that
// change driver state we additionally require FILE_WRITE_ACCESS so a handle
// opened read-only cannot mutate state.
//

// Return the driver protocol version and basic status. Read-only.
#define IOCTL_SECURECOMMS_GET_VERSION \
    CTL_CODE(SECURECOMMS_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)

// Echo: driver copies (bounded) input payload back to the output buffer.
// Demonstrates safe, length-checked METHOD_BUFFERED round-tripping.
#define IOCTL_SECURECOMMS_ECHO \
    CTL_CODE(SECURECOMMS_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Enable/disable process-creation monitoring. Mutates global state.
#define IOCTL_SECURECOMMS_SET_MONITORING \
    CTL_CODE(SECURECOMMS_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)

// Drain buffered process-creation events into the caller's output buffer.
#define IOCTL_SECURECOMMS_DRAIN_EVENTS \
    CTL_CODE(SECURECOMMS_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)

//
// ---- Request / response payloads ------------------------------------------
//
// All multi-field structures are explicitly packed/aligned to a fixed layout.
// We use fixed-width types so the 32-bit user-mode client and 64-bit kernel
// agree byte-for-byte (WOW64 compatibility).
//

#include <pshpack8.h>

//
// IOCTL_SECURECOMMS_GET_VERSION
//   Input:  none
//   Output: SECURECOMMS_VERSION_INFO
//
typedef struct _SECURECOMMS_VERSION_INFO {
    unsigned long   ProtocolVersion;   // == SECURECOMMS_PROTOCOL_VERSION
    unsigned long   MonitoringEnabled; // 0/1 current monitoring state
    unsigned long   EventsBuffered;    // number of events waiting to drain
    unsigned long   EventsDropped;     // events lost to a full ring buffer
} SECURECOMMS_VERSION_INFO, *PSECURECOMMS_VERSION_INFO;

//
// IOCTL_SECURECOMMS_ECHO
//   Input:  SECURECOMMS_ECHO_REQUEST (header + PayloadLength bytes)
//   Output: the same PayloadLength bytes copied back
//
typedef struct _SECURECOMMS_ECHO_REQUEST {
    unsigned long   ProtocolVersion;   // must match SECURECOMMS_PROTOCOL_VERSION
    unsigned long   PayloadLength;     // <= SECURECOMMS_MAX_PAYLOAD
    unsigned char   Payload[1];        // variable-length; PayloadLength bytes
} SECURECOMMS_ECHO_REQUEST, *PSECURECOMMS_ECHO_REQUEST;

//
// IOCTL_SECURECOMMS_SET_MONITORING
//   Input:  SECURECOMMS_SET_MONITORING_REQUEST
//   Output: none
//
typedef struct _SECURECOMMS_SET_MONITORING_REQUEST {
    unsigned long   ProtocolVersion;   // must match SECURECOMMS_PROTOCOL_VERSION
    unsigned long   Enable;            // 0 = disable, non-zero = enable
} SECURECOMMS_SET_MONITORING_REQUEST, *PSECURECOMMS_SET_MONITORING_REQUEST;

//
// One process-creation event as reported to user mode.
//
typedef struct _SECURECOMMS_PROCESS_EVENT {
    unsigned long long  ProcessId;                      // HANDLE-width, as u64
    unsigned long long  ParentProcessId;                // creating process id
    unsigned long long  CreatingThreadProcessId;        // subsystem/thread info
    unsigned long       IsCreate;                       // 1 create, 0 exit
    unsigned long       ImageNameLengthCch;             // valid chars in ImageName
    wchar_t             ImageName[SECURECOMMS_IMAGE_NAME_CCH]; // NUL-terminated
} SECURECOMMS_PROCESS_EVENT, *PSECURECOMMS_PROCESS_EVENT;

//
// IOCTL_SECURECOMMS_DRAIN_EVENTS
//   Input:  none
//   Output: SECURECOMMS_DRAIN_EVENTS_RESULT (header + EventCount events)
//
typedef struct _SECURECOMMS_DRAIN_EVENTS_RESULT {
    unsigned long               EventCount;   // events actually returned
    unsigned long               Remaining;    // events still buffered
    SECURECOMMS_PROCESS_EVENT   Events[1];     // variable-length array
} SECURECOMMS_DRAIN_EVENTS_RESULT, *PSECURECOMMS_DRAIN_EVENTS_RESULT;

#include <poppack.h>

#endif // _SECURECOMMS_PUBLIC_H_
