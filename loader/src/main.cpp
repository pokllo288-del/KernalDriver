/*++

    FOR EDUCATIONAL PURPOSES ONLY.

    Starlite — a themed Dear ImGui front-end for our educational driver.

    What it is:
      * A polished loader UI (welcome animation, falling-star background,
        a game card for "CS2", and a LOAD button).
      * The LOAD button starts our kernel driver through the Windows Service
        Control Manager (the same supported path as `sc start`). It performs
        NO game memory access and contains NO cheat/anti-cheat logic yet — it
        is purely the shell, exactly as requested.

    Backend: Win32 + Direct3D 11 + Dear ImGui. This file mirrors the official
    Dear ImGui "example_win32_directx11" scaffolding and adds the Starlite UI
    in RenderUI(). Vendor Dear ImGui as described in loader/README.md.

--*/

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <string>

#include "DriverControl.h"

// -------- Driver identity (matches the installed kernel service) -----------
static const wchar_t* kServiceName = L"SecureComms";
static const wchar_t* kDisplayName = L"Starlite Guard (educational)";

// -------- D3D11 globals (from the standard ImGui example) ------------------
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ===========================================================================
//  Theme
// ===========================================================================
static void ApplyStarliteTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 14.0f;
    s.FrameRounding     = 10.0f;
    s.GrabRounding      = 10.0f;
    s.PopupRounding     = 10.0f;
    s.ChildRounding     = 12.0f;
    s.WindowBorderSize  = 0.0f;
    s.FramePadding      = ImVec2(14, 10);
    s.ItemSpacing       = ImVec2(12, 12);

    ImVec4* c = s.Colors;
    const ImVec4 accent   = ImVec4(0.42f, 0.62f, 1.00f, 1.00f); // starlight blue
    const ImVec4 accentHi = ImVec4(0.56f, 0.74f, 1.00f, 1.00f);

    c[ImGuiCol_WindowBg]        = ImVec4(0.05f, 0.06f, 0.10f, 0.00f); // transparent; we paint bg
    c[ImGuiCol_ChildBg]         = ImVec4(0.09f, 0.11f, 0.18f, 0.72f);
    c[ImGuiCol_Text]            = ImVec4(0.90f, 0.93f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.55f, 0.60f, 0.72f, 1.00f);
    c[ImGuiCol_Button]          = accent;
    c[ImGuiCol_ButtonHovered]   = accentHi;
    c[ImGuiCol_ButtonActive]    = ImVec4(0.34f, 0.52f, 0.92f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.14f, 0.17f, 0.26f, 0.80f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.18f, 0.22f, 0.34f, 0.90f);
    c[ImGuiCol_Border]          = ImVec4(0.26f, 0.34f, 0.55f, 0.35f);
}

// ===========================================================================
//  Falling-star background
// ===========================================================================
struct Star {
    ImVec2 pos;
    float  speed;    // px/sec downward
    float  radius;
    float  twinkle;  // phase for alpha shimmer
    float  drift;    // slight horizontal sway
};

class StarField {
public:
    void Init(int count, float w, float h)
    {
        m_stars.clear();
        m_stars.reserve(count);
        for (int i = 0; i < count; ++i) m_stars.push_back(Spawn(w, h, true));
    }

    void Update(float dt, float w, float h)
    {
        for (auto& st : m_stars) {
            st.pos.y += st.speed * dt;
            st.pos.x += std::sin(st.pos.y * 0.01f + st.drift) * 8.0f * dt;
            st.twinkle += dt * 2.0f;
            if (st.pos.y - st.radius > h) st = Spawn(w, h, false); // respawn at top
        }
    }

    void Draw(ImDrawList* dl, float w, float h)
    {
        // Soft vertical gradient backdrop (deep space).
        dl->AddRectFilledMultiColor(
            ImVec2(0, 0), ImVec2(w, h),
            IM_COL32(9, 10, 20, 255),  IM_COL32(9, 10, 20, 255),
            IM_COL32(18, 16, 40, 255), IM_COL32(14, 20, 46, 255));

        for (const auto& st : m_stars) {
            float a = 0.45f + 0.55f * (0.5f + 0.5f * std::sin(st.twinkle));
            ImU32 col = IM_COL32(180, 205, 255, (int)(a * 255));
            dl->AddCircleFilled(st.pos, st.radius, col, 8);
            if (st.radius > 1.6f) // glow for larger stars
                dl->AddCircleFilled(st.pos, st.radius * 2.2f,
                                    IM_COL32(120, 160, 255, (int)(a * 40)), 12);
        }
    }

private:
    static Star Spawn(float w, float h, bool anywhere)
    {
        Star s;
        s.pos     = ImVec2(RandF(0, w), anywhere ? RandF(0, h) : RandF(-40, -2));
        s.speed   = RandF(18.0f, 70.0f);
        s.radius  = RandF(0.7f, 2.6f);
        s.twinkle = RandF(0.0f, 6.28f);
        s.drift   = RandF(0.0f, 6.28f);
        return s;
    }
    static float RandF(float a, float b)
    {
        return a + (b - a) * (float)std::rand() / (float)RAND_MAX;
    }
    std::vector<Star> m_stars;
};

// ===========================================================================
//  UI state
// ===========================================================================
enum class Screen { Welcome, Menu, Loading, Loaded };

struct AppState {
    Screen       screen = Screen::Welcome;
    float        t = 0.0f;          // total elapsed seconds
    float        screenT = 0.0f;    // seconds since entering current screen
    float        loadProgress = 0.0f;
    std::wstring sysPath = L"SecureComms.sys"; // path passed to Install()
    std::string  statusLine = "Idle";
    bool         statusError = false;
    DriverControl driver{ kServiceName, kDisplayName };

};

// Draw a stylized circular "CS2" avatar placeholder (swap for a real texture
// via ImGui::Image if you load one — see README). Avoids shipping game art.
static void DrawAvatar(ImDrawList* dl, ImVec2 center, float r)
{
    dl->AddCircleFilled(center, r + 4, IM_COL32(70, 110, 220, 90), 48);
    dl->AddCircleFilled(center, r,     IM_COL32(24, 28, 44, 255), 48);
    dl->AddCircle(center, r, IM_COL32(120, 160, 255, 220), 48, 2.5f);
    // "CS2" text centered.
    ImGui::PushFont(nullptr);
    const char* label = "CS2";
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
                IM_COL32(220, 232, 255, 255), label);
    ImGui::PopFont();
}

static void SetStatus(AppState& app, const DriverResult& r)
{
    // DriverResult message is wide; convert to UTF-8 for ImGui.
    int n = WideCharToMultiByte(CP_UTF8, 0, r.message.c_str(), -1,
                                nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, r.message.c_str(), -1,
                            out.data(), n, nullptr, nullptr);
    app.statusLine  = out.empty() ? "(no message)" : out;
    app.statusError = !r.ok;
}

// Kick off the loading animation (which then installs+starts the driver).
static void BeginLoad(AppState& app)
{
    app.screen = Screen::Loading;
    app.screenT = 0.0f;
    app.loadProgress = 0.0f;
}

// ===========================================================================
//  Main UI
// ===========================================================================
static void RenderUI(AppState& app, StarField& stars, float dt)
{
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y;

    app.t += dt;
    app.screenT += dt;
    stars.Update(dt, W, H);

    // Full-screen background window (no decoration), painted first.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##bg", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    stars.Draw(bg, W, H);
    ImGui::End();
    ImGui::PopStyleColor();

    // ---- Welcome screen: fade "Welcome to Starlite" in, then advance -------
    if (app.screen == Screen::Welcome) {
        float fade = ImClamp(app.screenT / 1.2f, 0.0f, 1.0f);
        float out  = ImClamp((app.screenT - 2.6f) / 0.8f, 0.0f, 1.0f);
        float a = fade * (1.0f - out);
        const char* title = "Welcome to Starlite";
        const char* sub   = "Educational Anti-Cheat";

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.94f, 1.0f, a));
        ImVec2 ts = ImGui::CalcTextSize(title);
        bg->AddText(nullptr, 34.0f,
                    ImVec2(W * 0.5f - ts.x * 1.7f, H * 0.44f),
                    IM_COL32(210, 228, 255, (int)(a * 255)), title);
        ImVec2 ss = ImGui::CalcTextSize(sub);
        bg->AddText(ImVec2(W * 0.5f - ss.x * 0.5f, H * 0.44f + 44),
                    IM_COL32(140, 170, 240, (int)(a * 200)), sub);
        ImGui::PopStyleColor();

        if (app.screenT > 3.4f) { app.screen = Screen::Menu; app.screenT = 0; }
        return;
    }

    // ---- Main card ---------------------------------------------------------
    const ImVec2 cardSize(460, 520);
    ImGui::SetNextWindowPos(ImVec2((W - cardSize.x) * 0.5f, (H - cardSize.y) * 0.5f));
    ImGui::SetNextWindowSize(cardSize);
    ImGui::Begin("##card", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetWindowPos();

    // Header brand.
    ImGui::Dummy(ImVec2(0, 6));
    {
        const char* brand = "STARLITE";
        ImVec2 ts = ImGui::CalcTextSize(brand);
        ImGui::SetCursorPosX((cardSize.x - ts.x) * 0.5f);
        ImGui::TextColored(ImVec4(0.56f, 0.74f, 1.0f, 1.0f), "%s", brand);
    }

    // Avatar.
    ImGui::Dummy(ImVec2(0, 14));
    DrawAvatar(dl, ImVec2(p0.x + cardSize.x * 0.5f, p0.y + 110), 46);
    ImGui::Dummy(ImVec2(0, 108));

    // Game title + subtitle.
    {
        const char* t1 = "CS2 Anti-Cheat";
        ImVec2 ts = ImGui::CalcTextSize(t1);
        ImGui::SetCursorPosX((cardSize.x - ts.x) * 0.5f);
        ImGui::Text("%s", t1);

        const char* t2 = "Counter-Strike 2  \xE2\x80\xA2  Protected Session";
        ImVec2 s2 = ImGui::CalcTextSize(t2);
        ImGui::SetCursorPosX((cardSize.x - s2.x) * 0.5f);
        ImGui::TextDisabled("%s", t2);
    }

    ImGui::Dummy(ImVec2(0, 18));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 8));

    // Driver state chip.
    DriverState st = app.driver.Query();
    const char* stateText = "Unknown";
    ImVec4 stateCol = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    switch (st) {
        case DriverState::Running:      stateText = "Protection ACTIVE"; stateCol = ImVec4(0.35f,0.85f,0.5f,1); break;
        case DriverState::Stopped:      stateText = "Installed - stopped"; stateCol = ImVec4(0.95f,0.8f,0.35f,1); break;
        case DriverState::NotInstalled: stateText = "Not installed";      stateCol = ImVec4(0.9f,0.5f,0.5f,1); break;
        default:                        stateText = "State unknown";      break;
    }
    ImGui::TextColored(stateCol, "  %s", stateText);
    ImGui::Dummy(ImVec2(0, 12));

    // ---- LOAD button + loading animation ----------------------------------
    const ImVec2 btnSize(cardSize.x - 60, 52);
    ImGui::SetCursorPosX(30);

    if (app.screen == Screen::Loading) {
        app.loadProgress = ImMin(1.0f, app.loadProgress + dt * 0.6f);
        char buf[64];
        snprintf(buf, sizeof(buf), "Initializing...  %d%%", (int)(app.loadProgress * 100));
        ImGui::ProgressBar(app.loadProgress, btnSize, buf);
        if (app.loadProgress >= 1.0f) {
            // Install (idempotent) then start the driver via SCM.
            if (!DriverControl::IsElevated()) {
                app.statusLine = "Run Starlite as Administrator to load the driver.";
                app.statusError = true;
                app.screen = Screen::Menu;
            } else {
                DriverResult ins = app.driver.Install(app.sysPath);
                if (!ins.ok && ins.win32Error != ERROR_SERVICE_EXISTS) {
                    SetStatus(app, ins);
                    app.screen = Screen::Menu;
                } else {
                    DriverResult start = app.driver.Start();
                    SetStatus(app, start);
                    app.screen = start.ok ? Screen::Loaded : Screen::Menu;
                }
            }
        }
    } else {
        const char* label = (app.screen == Screen::Loaded) ? "UNLOAD" : "LOAD";
        // Subtle pulse on the accent while idle.
        float pulse = 0.5f + 0.5f * std::sin(app.t * 2.5f);
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImVec4(0.36f + 0.06f * pulse, 0.56f + 0.06f * pulse, 1.0f, 1.0f));
        if (ImGui::Button(label, btnSize)) {
            if (app.screen == Screen::Loaded) {
                DriverResult stop = app.driver.Stop();
                SetStatus(app, stop);
                app.screen = Screen::Menu;
            } else {
                // Just try to load. If the driver is unsigned and Test Mode is
                // off, Start() fails and the error shows in the status line.
                BeginLoad(app);
            }
        }
        ImGui::PopStyleColor();
    }

    // Status line.
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::PushTextWrapPos(cardSize.x - 30);
    ImGui::TextColored(app.statusError ? ImVec4(0.95f, 0.5f, 0.5f, 1.0f)
                                       : ImVec4(0.6f, 0.7f, 0.85f, 1.0f),
                       "%s", app.statusLine.c_str());
    ImGui::PopTextWrapPos();

    // Footer.
    ImGui::SetCursorPosY(cardSize.y - 34);
    ImGui::TextDisabled("Educational build - no active detection yet");

    ImGui::End();
}

// ===========================================================================
//  WinMain + D3D11 plumbing (standard Dear ImGui example)
// ===========================================================================
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       hInst, nullptr, nullptr, nullptr, nullptr,
                       L"StarliteLoader", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Starlite",
        WS_OVERLAPPEDWINDOW, 100, 100, 720, 720,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyStarliteTheme();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    AppState app;
    StarField stars;
    stars.Init(160, 720.0f, 720.0f);

    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - prev.QuadPart) / (float)freq.QuadPart;
        prev = now;
        if (dt > 0.1f) dt = 0.1f;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderUI(app, stars, dt);

        ImGui::Render();
        const float clear[4] = { 0.03f, 0.03f, 0.06f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0); // vsync
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &fl, &g_pd3dDeviceContext) != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* back = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_pd3dDevice->CreateRenderTargetView(back, nullptr, &g_mainRenderTargetView);
        back->Release();
    }
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                                        DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
