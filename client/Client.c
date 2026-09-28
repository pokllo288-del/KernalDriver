/*++

Module Name:

    Client.c

Abstract:

    Minimal user-mode client for the SecureComms driver. Demonstrates opening
    the device, querying the version, echoing a payload, enabling process
    monitoring, and draining buffered events.

    Build (Developer Command Prompt):
        cl /W4 /Zi Client.c /I..\inc /link Advapi32.lib

    Run elevated: the device's SDDL grants full access only to SYSTEM and
    Administrators, so a non-elevated open of the write IOCTLs will fail with
    ERROR_ACCESS_DENIED by design.

Environment:

    User mode.

--*/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "../inc/SecureComms_Public.h"

static HANDLE OpenDevice(void)
{
    HANDLE h = CreateFileW(
                   SECURECOMMS_WIN32_NAME,
                   GENERIC_READ | GENERIC_WRITE,
                   0,                       // no sharing
                   NULL,
                   OPEN_EXISTING,
                   FILE_ATTRIBUTE_NORMAL,
                   NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateFile failed: %lu\n", GetLastError());
    }
    return h;
}

static BOOL QueryVersion(HANDLE h)
{
    SECURECOMMS_VERSION_INFO info = { 0 };
    DWORD returned = 0;

    if (!DeviceIoControl(h, IOCTL_SECURECOMMS_GET_VERSION,
                         NULL, 0,
                         &info, sizeof(info),
                         &returned, NULL)) {
        fprintf(stderr, "GET_VERSION failed: %lu\n", GetLastError());
        return FALSE;
    }
    printf("Protocol=0x%08lX Monitoring=%lu Buffered=%lu Dropped=%lu\n",
           info.ProtocolVersion, info.MonitoringEnabled,
           info.EventsBuffered, info.EventsDropped);
    return TRUE;
}

static BOOL Echo(HANDLE h, const char* text)
{
    BYTE inBuf[sizeof(SECURECOMMS_ECHO_REQUEST) + SECURECOMMS_MAX_PAYLOAD];
    BYTE outBuf[SECURECOMMS_MAX_PAYLOAD];
    PSECURECOMMS_ECHO_REQUEST req = (PSECURECOMMS_ECHO_REQUEST)inBuf;
    DWORD payloadLen = (DWORD)strlen(text);
    DWORD returned = 0;

    if (payloadLen > SECURECOMMS_MAX_PAYLOAD) {
        payloadLen = SECURECOMMS_MAX_PAYLOAD;
    }

    req->ProtocolVersion = SECURECOMMS_PROTOCOL_VERSION;
    req->PayloadLength = payloadLen;
    memcpy(req->Payload, text, payloadLen);

    if (!DeviceIoControl(h, IOCTL_SECURECOMMS_ECHO,
                         inBuf, FIELD_OFFSET(SECURECOMMS_ECHO_REQUEST, Payload) + payloadLen,
                         outBuf, sizeof(outBuf),
                         &returned, NULL)) {
        fprintf(stderr, "ECHO failed: %lu\n", GetLastError());
        return FALSE;
    }
    printf("Echoed %lu bytes: %.*s\n", returned, (int)returned, (char*)outBuf);
    return TRUE;
}

static BOOL SetMonitoring(HANDLE h, BOOL enable)
{
    SECURECOMMS_SET_MONITORING_REQUEST req = { 0 };
    DWORD returned = 0;

    req.ProtocolVersion = SECURECOMMS_PROTOCOL_VERSION;
    req.Enable = enable ? 1 : 0;

    if (!DeviceIoControl(h, IOCTL_SECURECOMMS_SET_MONITORING,
                         &req, sizeof(req),
                         NULL, 0,
                         &returned, NULL)) {
        fprintf(stderr, "SET_MONITORING failed: %lu\n", GetLastError());
        return FALSE;
    }
    printf("Monitoring %s\n", enable ? "enabled" : "disabled");
    return TRUE;
}

static BOOL DrainEvents(HANDLE h)
{
    BYTE outBuf[sizeof(SECURECOMMS_DRAIN_EVENTS_RESULT) +
                (SECURECOMMS_MAX_EVENTS - 1) * sizeof(SECURECOMMS_PROCESS_EVENT)];
    PSECURECOMMS_DRAIN_EVENTS_RESULT result =
        (PSECURECOMMS_DRAIN_EVENTS_RESULT)outBuf;
    DWORD returned = 0;
    ULONG i;

    if (!DeviceIoControl(h, IOCTL_SECURECOMMS_DRAIN_EVENTS,
                         NULL, 0,
                         outBuf, sizeof(outBuf),
                         &returned, NULL)) {
        fprintf(stderr, "DRAIN_EVENTS failed: %lu\n", GetLastError());
        return FALSE;
    }

    printf("Drained %lu event(s), %lu remaining:\n",
           result->EventCount, result->Remaining);
    for (i = 0; i < result->EventCount; ++i) {
        PSECURECOMMS_PROCESS_EVENT ev = &result->Events[i];
        printf("  [%s] pid=%llu parent=%llu image=%ls\n",
               ev->IsCreate ? "CREATE" : "EXIT ",
               ev->ProcessId, ev->ParentProcessId, ev->ImageName);
    }
    return TRUE;
}

int main(int argc, char** argv)
{
    HANDLE h;
    int seconds = (argc > 1) ? atoi(argv[1]) : 5;

    h = OpenDevice();
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }

    QueryVersion(h);
    Echo(h, "hello secure world");

    SetMonitoring(h, TRUE);
    printf("Monitoring for %d seconds; launch some processes...\n", seconds);
    Sleep((DWORD)seconds * 1000);
    DrainEvents(h);
    SetMonitoring(h, FALSE);

    CloseHandle(h);
    return 0;
}
