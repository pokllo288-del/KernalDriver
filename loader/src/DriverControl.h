/*++

    FOR EDUCATIONAL PURPOSES ONLY.

    DriverControl.h

    Thin wrapper over the Windows Service Control Manager (SCM) for
    installing / starting / stopping / removing a *kernel service*.

    IMPORTANT: this uses only the documented, supported path
    (CreateService/StartService with SERVICE_KERNEL_DRIVER). It loads a
    driver that Windows will accept — i.e. a properly signed .sys, or any
    .sys on a machine with test-signing enabled. It deliberately does NOT
    implement any "manual mapping" / unsigned-driver-loading trick.

--*/
#pragma once
#include <windows.h>
#include <string>

enum class DriverState {
    Unknown,
    NotInstalled,
    Stopped,
    Running,
};

struct DriverResult {
    bool        ok = false;
    DWORD       win32Error = ERROR_SUCCESS;
    std::wstring message;
};

// All calls require the process to be elevated (Administrator).
class DriverControl {
public:
    DriverControl(std::wstring serviceName, std::wstring displayName)
        : m_service(std::move(serviceName)), m_display(std::move(displayName)) {}

    // Create the kernel service pointing at an on-disk .sys (demand start).
    DriverResult Install(const std::wstring& sysPath);

    // Start / stop the already-installed service.
    DriverResult Start();
    DriverResult Stop();

    // Remove the service definition.
    DriverResult Remove();

    // Query current state (never throws).
    DriverState  Query();

    static bool  IsElevated();

private:
    std::wstring m_service;
    std::wstring m_display;
};
