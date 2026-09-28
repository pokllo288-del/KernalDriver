/*++

    FOR EDUCATIONAL PURPOSES ONLY.

    DriverControl.cpp — SCM-based kernel-service control. See header for the
    important note on why this uses only the supported StartService path.

--*/
#include "DriverControl.h"
#include <vector>

// --- Minimal NtQuerySystemInformation declarations for code-integrity state.
//     (Not in the Windows SDK headers; declared locally as documented.) ------
extern "C" {
typedef LONG NTSTATUS_;
typedef NTSTATUS_ (NTAPI* PFN_NtQuerySystemInformation)(
    ULONG SystemInformationClass, PVOID SystemInformation,
    ULONG SystemInformationLength, PULONG ReturnLength);
}
namespace {
constexpr ULONG kSystemCodeIntegrityInformation = 103;
constexpr ULONG kCodeIntegrityOptionEnabled     = 0x01;
constexpr ULONG kCodeIntegrityOptionTestSign    = 0x02;
struct SYSTEM_CODEINTEGRITY_INFORMATION_ {
    ULONG Length;
    ULONG CodeIntegrityOptions;
};
}

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

bool DriverControl::IsTestSigningEnabled()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    auto fn = (PFN_NtQuerySystemInformation)
                  GetProcAddress(ntdll, "NtQuerySystemInformation");
    if (!fn) return false;

    SYSTEM_CODEINTEGRITY_INFORMATION_ info{};
    info.Length = sizeof(info);
    ULONG ret = 0;
    NTSTATUS_ st = fn(kSystemCodeIntegrityInformation, &info, sizeof(info), &ret);
    if (st < 0) return false; // query failed; assume "off" so we warn the user
    return (info.CodeIntegrityOptions & kCodeIntegrityOptionTestSign) != 0;
}

DriverResult DriverControl::EnableTestSigning()
{
    if (!IsElevated())
        return { false, ERROR_ACCESS_DENIED,
                 L"Run as Administrator to enable Test Mode." };

    // Invoke the documented bcdedit command. CreateProcess so we can wait and
    // read the exit code; no shell needed.
    wchar_t cmd[] = L"bcdedit.exe /set testsigning on";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return Fail(GetLastError(), L"CreateProcess(bcdedit)");

    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (code == 0)
        return { true, ERROR_SUCCESS,
                 L"Test Mode set. REBOOT for it to take effect. "
                 L"(If it failed, disable Secure Boot in UEFI first.)" };
    return { false, ERROR_INVALID_FUNCTION,
             L"bcdedit failed - likely Secure Boot is on. Disable Secure Boot "
             L"in your UEFI/BIOS, then try again." };
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
        result = (err == ERROR_SERVICE_ALREADY_RUNNING)
                     ? Ok(L"Driver already running")
                     : Fail(err, L"StartService");
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
