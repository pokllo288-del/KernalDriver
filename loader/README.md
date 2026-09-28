# Starlite — Loader UI (educational)

A polished **Dear ImGui** front-end for the educational driver in this repo.

> ⚠️ **FOR EDUCATIONAL PURPOSES ONLY.** Starlite is a *UI shell*: a welcome
> animation, a falling-star background, a game card, and a **Load** button. The
> button installs/starts our kernel driver through the **Windows Service
> Control Manager** — the same supported path as `sc start`. It does **not**
> read or modify any game's memory and contains **no** cheat or anti-cheat
> detection logic yet. Run it only on a machine you own / are authorized to use.

## What it shows

1. **Welcome** — "Welcome to Starlite" fades in over the star field, then
   auto-advances.
2. **Menu** — a centered card with the STARLITE brand, a circular **CS2**
   avatar placeholder, the title **"CS2 Anti-Cheat"**, a live driver-state
   chip, and a pulsing **LOAD** button.
3. **Loading** — an animated progress bar, then it calls the SCM to
   install (idempotent) + start the driver.
4. **Loaded** — state chip flips to *Protection ACTIVE*; the button becomes
   **UNLOAD** (stops the service).

The whole background is a live **falling-star** particle system (fits the
"Starlite" name), redrawn every frame with a twinkle/glow.

## Files

| File | Role |
|------|------|
| `src/main.cpp`         | Win32 + D3D11 + ImGui app; all Starlite UI in `RenderUI()`. |
| `src/DriverControl.*`  | SCM wrapper: install / start / stop / remove / query. Documented `StartService` path only — no manual/unsigned driver mapping. |
| `app.manifest`         | Requests Administrator elevation (needed for SCM). |

## Getting Dear ImGui

Starlite is written against upstream Dear ImGui. Vendor it next to the loader:

```
cd loader
git clone --depth 1 https://github.com/ocornut/imgui third_party/imgui
```

You need these from that clone, on your include path / in your build:
`imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp`
`backends/imgui_impl_win32.cpp backends/imgui_impl_dx11.cpp`
plus the matching headers.

## Building (Developer Command Prompt, x64)

```
cd loader
cl /std:c++17 /EHsc /DUNICODE /D_UNICODE ^
   /I third_party\imgui /I third_party\imgui\backends /I src ^
   src\main.cpp src\DriverControl.cpp ^
   third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp ^
   third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp ^
   third_party\imgui\backends\imgui_impl_win32.cpp ^
   third_party\imgui\backends\imgui_impl_dx11.cpp ^
   /link d3d11.lib dxgi.lib d3dcompiler.lib Advapi32.lib ^
   /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:app.manifest ^
   /OUT:Starlite.exe
```

(A CMake/`.vcxproj` works too — the include dirs, the six ImGui source files,
the four `.lib`s, and the embedded manifest are all that matter.)

## Running

1. Build `SecureComms.sys` (see the repo root README) and put it next to
   `Starlite.exe`, or edit `app.sysPath` in `main.cpp` to its full path.
2. **Enable Test Mode** so the unsigned driver can load — see
   [`TESTMODE.md`](TESTMODE.md). Starlite also shows an amber banner with an
   **"Enable Test Mode"** button whenever it detects Test Mode is off; click it,
   then reboot.
3. On a **test VM**, launch `Starlite.exe` — it self-elevates via the manifest.
4. Click **LOAD**. The state chip should read *Protection ACTIVE*; **UNLOAD**
   stops it.

> The driver is unsigned, so it will only load with **Test Mode on** (or a
> one-time "disable driver signature enforcement" boot). Starlite detects this
> via `NtQuerySystemInformation(SystemCodeIntegrityInformation)` and warns you.
> This is the documented developer workflow — it affects only your own test
> machine, not anyone else's security.

## Swapping in a real CS2 avatar

The avatar is a drawn placeholder so no game art is shipped. To use an image:
load a texture into an `ID3D11ShaderResourceView*` (e.g. via `stb_image`) and
replace `DrawAvatar()` with `ImGui::Image((ImTextureID)srv, size)`.

## Boundaries (why it's built this way)

- Loads drivers **only** through the SCM (`CreateService`/`StartService`).
  No manual mapping, no signature-bypass — those are cheat-scene techniques.
- No game process is opened, read, or written.
- "Anti-cheat" here is branding on a shell; detection logic is intentionally
  absent, as requested.
