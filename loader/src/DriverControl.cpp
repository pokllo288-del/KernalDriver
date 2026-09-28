/*++

    FOR EDUCATIONAL PURPOSES ONLY.

    DriverControl.cpp — SCM-based kernel-service control. See header for the
    important note on why this uses only the supported StartService path.

--*/
#include "DriverControl.h"
#include <vector>

namespace {

// Format a human-readable message for a Win32 error.
std::wstring FormatWin32(DWORD err)
{
    if (err == ERROR_SUCCESS) return L"OK";
    LPWSTR buf = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&buf, 0, nullptr);
    std::wstring s = (n && buf) ? std::wstring(buf, n) : L"(unknown error)";
    if (buf) LocalFree(buf);
    // Trim trailing CR/LF.
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

DriverResult Fail(DWORD err, const wchar_t* what)
{
    return { false, err, std::wstring(what) + L": " + FormatWin32(err) };
}

DriverResult Ok(const wchar_t* what)
{
    return { true, ERROR_SUCCESS, what };
}

} // namespace

bool DriverControl::IsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elev{};
    DWORD sz = sizeof(elev);
    bool elevated = false;
    if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz))
        elevated = elev.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

DriverResult DriverControl::Install(const std::wstring& sysPath)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return Fail(GetLastError(), L"OpenSCManager");

    SC_HANDLE svc = CreateServiceW(
        scm, m_service.c_str(), m_display.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_KERNEL_DRIVER,      // kernel driver
        SERVICE_DEMAND_START,       // started on request
        SERVICE_ERROR_NORMAL,
        sysPath.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr);

    DWORD err = GetLastError();
    if (!svc && err == ERROR_SERVICE_EXISTS) {
        CloseServiceHandle(scm);
        return Ok(L"Service already installed");
    }
    if (!svc) {
        CloseServiceHandle(scm);
        return Fail(err, L"CreateService");
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return Ok(L"Driver service installed");
}

DriverResult DriverControl::Start()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return Fail(GetLastError(), L"OpenSCManager");

    SC_HANDLE svc = OpenServiceW(scm, m_service.c_str(), SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        return Fail(err, L"OpenService");
    }

    DriverResult result;
    if (StartServiceW(svc, 0, nullptr)) {
        result = Ok(L"Driver started");
    } else {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING) {
            result = Ok(L"Driver already running");
        } else {
            result = Fail(err, L"StartService");
            // ERROR_INVALID_IMAGE_HASH (577): unsigned driver + Test Mode off.
            if (err == 577)
                result.message += L"  ->  The driver is unsigned. Enable Test "
                                  L"Mode (bcdedit /set testsigning on) and "
                                  L"reboot. See loader/TESTMODE.md.";
        }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return result;
}

DriverResult DriverControl::Stop()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return Fail(GetLastError(), L"OpenSCManager");

    SC_HANDLE svc = OpenServiceW(scm, m_service.c_str(), SERVICE_STOP);
    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        return Fail(err, L"OpenService");
    }

    SERVICE_STATUS status{};
    DriverResult result;
    if (ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
        result = Ok(L"Driver stopped");
    } else {
        DWORD err = GetLastError();
        result = (err == ERROR_SERVICE_NOT_ACTIVE)
                     ? Ok(L"Driver not running")
                     : Fail(err, L"ControlService(STOP)");
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return result;
}

DriverResult DriverControl::Remove()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return Fail(GetLastError(), L"OpenSCManager");

    SC_HANDLE svc = OpenServiceW(scm, m_service.c_str(), DELETE);
    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        return Fail(err, L"OpenService");
    }

    DriverResult result = DeleteService(svc)
                              ? Ok(L"Driver service removed")
                              : Fail(GetLastError(), L"DeleteService");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return result;
}

DriverState DriverControl::Query()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return DriverState::Unknown;

    SC_HANDLE svc = OpenServiceW(scm, m_service.c_str(), SERVICE_QUERY_STATUS);
    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        return (err == ERROR_SERVICE_DOES_NOT_EXIST)
                   ? DriverState::NotInstalled : DriverState::Unknown;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    DriverState state = DriverState::Unknown;
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                             (LPBYTE)&ssp, sizeof(ssp), &needed)) {
        state = (ssp.dwCurrentState == SERVICE_RUNNING)
                    ? DriverState::Running : DriverState::Stopped;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return state;
}
