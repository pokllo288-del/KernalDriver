"""Foresight launcher UI (tkinter, standard library only).

Screens:
  1. Library  - the Minecraft game card with Activate / Load.
  2. Versions - Minecraft 1.21.11 with a Load button that downloads every file.
  3. Account  - pick a player name, or sign in with a Microsoft account.
"""

from __future__ import annotations

import json
import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
import webbrowser
from pathlib import Path
from tkinter import ttk

from .auth import (Account, AuthCancelled, AuthError, DeviceCode, MicrosoftAuth,
                   offline_account)
from .minecraft import DownloadCancelled, MinecraftInstaller, Progress

APP_NAME = "Foresight"
GAME_VERSION = "1.21.11"

# Palette
BG = "#0e0f12"
SURFACE = "#16181d"
SURFACE_HI = "#1d2027"
BORDER = "#262a33"
TEXT = "#e8eaed"
MUTED = "#8a909c"
ACCENT = "#3ddc84"
ACCENT_HI = "#5ae89a"
ACCENT_TEXT = "#0b1a10"
DANGER = "#ff6b6b"

FONT = "Segoe UI" if sys.platform == "win32" else "Helvetica"
MONO = "Consolas" if sys.platform == "win32" else "Courier"


def font(size: int, weight: str = "normal") -> tuple:
    return (FONT, size, weight)


class Button(tk.Label):
    """Flat, rounded-looking button built on a Label so it styles the same on every OS."""

    def __init__(self, master, text: str, command, primary: bool = True, width: int = 14):
        self.primary = primary
        self.command = command
        self.enabled = True
        super().__init__(master, text=text, font=font(11, "bold"), width=width,
                         padx=10, pady=9, cursor="hand2")
        self._paint()
        self.bind("<Enter>", lambda _e: self._paint(hover=True))
        self.bind("<Leave>", lambda _e: self._paint())
        self.bind("<Button-1>", self._click)

    def _paint(self, hover: bool = False) -> None:
        if not self.enabled:
            self.configure(bg=SURFACE_HI, fg="#4b505b", cursor="arrow")
        elif self.primary:
            self.configure(bg=ACCENT_HI if hover else ACCENT, fg=ACCENT_TEXT, cursor="hand2")
        else:
            self.configure(bg=BORDER if hover else SURFACE_HI, fg=TEXT, cursor="hand2")

    def _click(self, _event) -> None:
        if self.enabled and self.command:
            self.command()

    def set_enabled(self, enabled: bool) -> None:
        self.enabled = enabled
        self._paint()

    def set_text(self, text: str) -> None:
        self.configure(text=text)


class MinecraftButton(tk.Canvas):
    """A button drawn like Minecraft's menu buttons: grey stone, bevelled edges, shadowed text."""

    FACE, FACE_HOVER = "#6f6f6f", "#7d7d7d"
    LIGHT, DARK, OUTLINE = "#a9a9a9", "#4b4b4b", "#000000"

    def __init__(self, master, text: str, command, width: int = 320, height: int = 44,
                 microsoft_logo: bool = False, bg: str = SURFACE):
        super().__init__(master, width=width, height=height, bg=bg, highlightthickness=0,
                         cursor="hand2")
        self.text, self.command, self.logo = text, command, microsoft_logo
        self.w, self.h = width, height
        self.enabled = True
        self._draw()
        self.bind("<Enter>", lambda _e: self._draw(hover=True))
        self.bind("<Leave>", lambda _e: self._draw())
        self.bind("<Button-1>", lambda _e: self.enabled and self.command())

    def _draw(self, hover: bool = False) -> None:
        self.delete("all")
        w, h = self.w, self.h
        hover = hover and self.enabled
        self.create_rectangle(0, 0, w - 1, h - 1, fill=self.OUTLINE, outline=self.OUTLINE)
        face = "#4a4a4a" if not self.enabled else (self.FACE_HOVER if hover else self.FACE)
        self.create_rectangle(2, 2, w - 3, h - 3, fill=face, outline=face)
        if self.enabled:
            # light top-left bevel, dark bottom-right bevel
            self.create_rectangle(2, 2, w - 3, 3, fill=self.LIGHT, outline="")
            self.create_rectangle(2, 2, 3, h - 3, fill=self.LIGHT, outline="")
            self.create_rectangle(2, h - 6, w - 3, h - 3, fill=self.DARK, outline="")
            self.create_rectangle(w - 4, 2, w - 3, h - 3, fill=self.DARK, outline="")
        if hover:
            self.create_rectangle(1, 1, w - 2, h - 2, outline="#ffffff", width=2)

        fg = "#ffffa0" if hover else ("#ffffff" if self.enabled else "#a0a0a0")
        text_font = (MONO, 12, "bold")
        cx, cy = w // 2, h // 2 - 1
        probe = self.create_text(0, 0, text=self.text, font=text_font, anchor="nw")
        x1, _, x2, _ = self.bbox(probe)
        self.delete(probe)
        text_w = x2 - x1
        logo_w = 28 if self.logo else 0
        start = cx - (text_w + logo_w) // 2
        if self.logo:
            s, gap, top = 7, 2, cy - 8
            for i, color in enumerate(("#f25022", "#7fba00", "#00a4ef", "#ffb900")):
                x = start + (i % 2) * (s + gap)
                y = top + (i // 2) * (s + gap)
                self.create_rectangle(x, y, x + s, y + s, fill=color, outline="")
        tx = start + logo_w
        self.create_text(tx + 2, cy + 2, text=self.text, font=text_font, fill="#3f3f3f", anchor="w")
        self.create_text(tx, cy, text=self.text, font=text_font, fill=fg, anchor="w")

    def set_enabled(self, enabled: bool) -> None:
        self.enabled = enabled
        self.configure(cursor="hand2" if enabled else "arrow")
        self._draw()


class ForesightApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.installer = MinecraftInstaller()
        self.settings_path = self.installer.install_dir / "launcher.json"
        self.settings = self._load_settings()
        self.events: "queue.Queue[tuple]" = queue.Queue()
        self.worker: threading.Thread | None = None
        self.account = Account.from_settings(self.settings.get("account"))
        self.auth_cancel = threading.Event()
        self.auth_worker: threading.Thread | None = None
        self.device_code: DeviceCode | None = None
        self.current_screen = None

        root.title(APP_NAME)
        root.geometry("760x600")
        root.minsize(640, 600)
        root.configure(bg=BG)
        root.protocol("WM_DELETE_WINDOW", self._on_close)

        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure("Foresight.Horizontal.TProgressbar", troughcolor=SURFACE_HI,
                        background=ACCENT, bordercolor=SURFACE_HI,
                        lightcolor=ACCENT, darkcolor=ACCENT, thickness=6)

        self._build_header()
        self.body = tk.Frame(root, bg=BG)
        self.body.pack(fill="both", expand=True, padx=32, pady=(8, 28))
        self.show_library()
        self.root.after(50, self._drain_events)

    # -- settings -----------------------------------------------------------

    def _load_settings(self) -> dict:
        try:
            return json.loads(self.settings_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def _save_settings(self) -> None:
        try:
            self.settings_path.parent.mkdir(parents=True, exist_ok=True)
            self.settings_path.write_text(json.dumps(self.settings, indent=2), encoding="utf-8")
            if os.name == "posix":
                os.chmod(self.settings_path, 0o600)  # may hold a Microsoft refresh token
        except OSError:
            pass

    @property
    def activated(self) -> bool:
        return bool(self.settings.get("minecraft_activated"))

    # -- layout helpers -----------------------------------------------------

    def _build_header(self) -> None:
        header = tk.Frame(self.root, bg=BG)
        header.pack(fill="x", padx=32, pady=(24, 8))
        tk.Label(header, text="◉", fg=ACCENT, bg=BG, font=font(12)).pack(side="left")
        tk.Label(header, text=f" {APP_NAME.upper()}", fg=TEXT, bg=BG,
                 font=font(13, "bold")).pack(side="left")
        tk.Label(header, text="Launcher", fg=MUTED, bg=BG, font=font(11)).pack(side="left", padx=8)

        self.account_chip = tk.Frame(header, bg=SURFACE_HI, cursor="hand2")
        self.account_chip.pack(side="right")
        self.chip_avatar = tk.Label(self.account_chip, font=font(9, "bold"), width=2,
                                    fg=ACCENT_TEXT, cursor="hand2")
        self.chip_avatar.pack(side="left", padx=(6, 0), pady=5)
        self.chip_name = tk.Label(self.account_chip, bg=SURFACE_HI, fg=TEXT, font=font(10),
                                  padx=8, cursor="hand2")
        self.chip_name.pack(side="left", padx=(0, 4))
        for widget in (self.account_chip, self.chip_avatar, self.chip_name):
            widget.bind("<Button-1>", lambda _e: self.show_account())
        self._refresh_account_chip()

    def _refresh_account_chip(self) -> None:
        if self.account:
            self.chip_avatar.configure(text=self.account.name[0].upper(),
                                       bg=ACCENT if self.account.type == "microsoft" else "#c9a227")
            self.chip_name.configure(text=self.account.name)
        else:
            self.chip_avatar.configure(text="?", bg=BORDER, fg=MUTED)
            self.chip_name.configure(text="Set player")
            return
        self.chip_avatar.configure(fg=ACCENT_TEXT)

    def _clear(self) -> None:
        self.current_screen = None
        for child in self.body.winfo_children():
            child.destroy()

    def _card(self, parent) -> tuple[tk.Frame, tk.Frame]:
        outer = tk.Frame(parent, bg=BORDER)
        inner = tk.Frame(outer, bg=SURFACE, padx=24, pady=22)
        inner.pack(fill="both", expand=True, padx=1, pady=1)
        return outer, inner

    @staticmethod
    def _game_icon(parent, size: int = 72) -> tk.Canvas:
        """A small pixel-grass block drawn on a canvas (no image assets needed)."""
        canvas = tk.Canvas(parent, width=size, height=size, bg=SURFACE, highlightthickness=0)
        px = size // 8
        dirt = ["#7a5230", "#8b5e3c", "#6e4a2b"]
        grass = ["#4caf50", "#5cbf60", "#43a047"]
        for y in range(8):
            for x in range(8):
                if y < 2 or (y == 2 and (x * 3) % 5 < 2):
                    color = grass[(x + y) % 3]
                else:
                    color = dirt[(x * 7 + y * 3) % 3]
                canvas.create_rectangle(x * px, y * px, (x + 1) * px, (y + 1) * px,
                                        fill=color, outline=color)
        return canvas

    # -- screen 1: library --------------------------------------------------

    def show_library(self) -> None:
        self._clear()
        self.current_screen = self.show_library
        tk.Label(self.body, text="Library", fg=TEXT, bg=BG, font=font(20, "bold"),
                 anchor="w").pack(fill="x")
        tk.Label(self.body, text="Select a game to get started.", fg=MUTED, bg=BG,
                 font=font(11), anchor="w").pack(fill="x", pady=(2, 18))

        outer, card = self._card(self.body)
        outer.pack(fill="x")

        self._game_icon(card).pack(side="left")

        info = tk.Frame(card, bg=SURFACE)
        info.pack(side="left", fill="both", expand=True, padx=20)
        tk.Label(info, text="Minecraft", fg=TEXT, bg=SURFACE, font=font(17, "bold"),
                 anchor="w").pack(fill="x")
        tk.Label(info, text="Java Edition", fg=MUTED, bg=SURFACE, font=font(11),
                 anchor="w").pack(fill="x")
        self.status_chip = tk.Label(info, font=font(9, "bold"), padx=8, pady=2)
        self.status_chip.pack(anchor="w", pady=(10, 0))

        actions = tk.Frame(card, bg=SURFACE)
        actions.pack(side="right")
        self.activate_btn = Button(actions, "", self._toggle_activate, primary=False, width=11)
        self.activate_btn.pack(side="left", padx=(0, 10))
        self.load_btn = Button(actions, "Load", self.show_versions, width=11)
        self.load_btn.pack(side="left")

        self._refresh_library()

    def _refresh_library(self) -> None:
        if self.activated:
            self.status_chip.configure(text="ACTIVE", bg="#14301f", fg=ACCENT)
            self.activate_btn.set_text("Deactivate")
        else:
            self.status_chip.configure(text="INACTIVE", bg=SURFACE_HI, fg=MUTED)
            self.activate_btn.set_text("Activate")
        self.load_btn.set_enabled(self.activated)

    def _toggle_activate(self) -> None:
        self.settings["minecraft_activated"] = not self.activated
        self._save_settings()
        self._refresh_library()

    # -- screen 2: versions -------------------------------------------------

    def show_versions(self) -> None:
        self._clear()
        self.current_screen = self.show_versions

        back = tk.Label(self.body, text="← Library", fg=MUTED, bg=BG, font=font(10),
                        cursor="hand2", anchor="w")
        back.pack(fill="x")
        back.bind("<Button-1>", lambda _e: self._go_back())
        self.back_link = back

        tk.Label(self.body, text="Minecraft", fg=TEXT, bg=BG, font=font(20, "bold"),
                 anchor="w").pack(fill="x", pady=(6, 0))
        tk.Label(self.body, text="Choose a version to install.", fg=MUTED, bg=BG,
                 font=font(11), anchor="w").pack(fill="x", pady=(2, 18))

        outer, card = self._card(self.body)
        outer.pack(fill="x")

        row = tk.Frame(card, bg=SURFACE)
        row.pack(fill="x")
        left = tk.Frame(row, bg=SURFACE)
        left.pack(side="left", fill="x", expand=True)
        tk.Label(left, text=GAME_VERSION, fg=TEXT, bg=SURFACE, font=font(17, "bold"),
                 anchor="w").pack(fill="x")
        tk.Label(left, text="Release  ·  Java Edition", fg=MUTED, bg=SURFACE, font=font(10),
                 anchor="w").pack(fill="x")

        self.version_btn = Button(row, "Load", self._on_load_version, width=12)
        self.version_btn.pack(side="right")

        self.progress = ttk.Progressbar(card, style="Foresight.Horizontal.TProgressbar",
                                        maximum=1000, mode="determinate")
        self.progress.pack(fill="x", pady=(20, 8))

        self.status = tk.Label(card, fg=MUTED, bg=SURFACE, font=font(10), anchor="w")
        self.status.pack(fill="x")

        footer = tk.Frame(self.body, bg=BG)
        footer.pack(fill="x", pady=(14, 0))
        tk.Label(footer, text=f"Install location: {self.installer.install_dir}", fg=MUTED,
                 bg=BG, font=font(9), anchor="w").pack(side="left")
        folder = tk.Label(footer, text="Open folder", fg=ACCENT, bg=BG, font=font(9),
                          cursor="hand2")
        folder.pack(side="right")
        folder.bind("<Button-1>", lambda _e: self._open_folder())

        if self.worker and self.worker.is_alive():
            self._set_busy(True)
        elif self.installer.is_installed(GAME_VERSION):
            self._show_installed()
        else:
            self.status.configure(text="Not installed  ·  about 500 MB download", fg=MUTED)

    def _go_back(self) -> None:
        if self.worker and self.worker.is_alive():
            return
        self.show_library()

    def _show_installed(self) -> None:
        self.progress.configure(value=1000)
        self.status.configure(text=f"✓  Minecraft {GAME_VERSION} is installed and verified.",
                              fg=ACCENT)
        self.version_btn.set_text("Verify")

    def _set_busy(self, busy: bool) -> None:
        self.version_btn.set_text("Cancel" if busy else "Load")
        self.version_btn.primary = not busy
        self.version_btn._paint()
        self.back_link.configure(fg="#4b505b" if busy else MUTED,
                                 cursor="arrow" if busy else "hand2")

    def _on_load_version(self) -> None:
        if self.worker and self.worker.is_alive():
            self.installer.cancel()
            self.status.configure(text="Cancelling…", fg=MUTED)
            return
        self._set_busy(True)
        self.progress.configure(value=0)
        self.status.configure(text="Starting…", fg=MUTED)
        self.worker = threading.Thread(target=self._install_worker, daemon=True)
        self.worker.start()

    def _install_worker(self) -> None:
        try:
            self.installer.install(GAME_VERSION, lambda p: self.events.put(("progress", p)))
            self.events.put(("done", None))
        except DownloadCancelled:
            self.events.put(("cancelled", None))
        except Exception as exc:  # surface any failure in the UI
            self.events.put(("error", str(exc)))

    # -- screen 3: account --------------------------------------------------

    def show_account(self) -> None:
        if self.worker and self.worker.is_alive():
            return  # keep the download screen visible while downloading
        if self.current_screen is not None and self.current_screen != self.show_account:
            self.return_to = self.current_screen
        self._clear()
        self.current_screen = self.show_account

        back = tk.Label(self.body, text="← Back", fg=MUTED, bg=BG, font=font(10),
                        cursor="hand2", anchor="w")
        back.pack(fill="x")
        back.bind("<Button-1>", lambda _e: self._leave_account())

        tk.Label(self.body, text="Account", fg=TEXT, bg=BG, font=font(20, "bold"),
                 anchor="w").pack(fill="x", pady=(6, 0))
        tk.Label(self.body, text="Choose how you appear in game.", fg=MUTED, bg=BG,
                 font=font(11), anchor="w").pack(fill="x", pady=(2, 16))

        if self.device_code:
            self._show_device_code(self.device_code)
            return

        if self.account:
            outer, card = self._card(self.body)
            card.configure(pady=14)
            outer.pack(fill="x", pady=(0, 12))
            kind = "Microsoft account" if self.account.type == "microsoft" else "Offline player"
            tk.Label(card, text=self.account.name, fg=TEXT, bg=SURFACE, font=font(15, "bold"),
                     anchor="w").pack(side="left")
            tk.Label(card, text=f"  ·  {kind}", fg=MUTED, bg=SURFACE, font=font(10)).pack(side="left")
            Button(card, "Sign out", self._sign_out, primary=False, width=9).pack(side="right")

        # Option 1: just a player name
        outer, card = self._card(self.body)
        outer.pack(fill="x")
        tk.Label(card, text="Player name", fg=TEXT, bg=SURFACE, font=font(12, "bold"),
                 anchor="w").pack(fill="x")
        tk.Label(card, text="Play offline with any name (3-16 letters, numbers or _).",
                 fg=MUTED, bg=SURFACE, font=font(10), anchor="w").pack(fill="x", pady=(2, 10))
        row = tk.Frame(card, bg=SURFACE)
        row.pack(fill="x")
        self.name_var = tk.StringVar(
            value=self.account.name if self.account and self.account.type == "offline" else "")
        entry_border = tk.Frame(row, bg=BORDER)
        entry_border.pack(side="left", fill="x", expand=True, padx=(0, 10))
        entry = tk.Entry(entry_border, textvariable=self.name_var, font=font(12), bg=SURFACE_HI,
                         fg=TEXT, insertbackground=TEXT, relief="flat", highlightthickness=0)
        entry.pack(fill="x", padx=1, pady=1, ipady=8, ipadx=8)
        entry.bind("<Return>", lambda _e: self._save_player_name())
        Button(row, "Save", self._save_player_name, width=8).pack(side="right")
        self.name_error = tk.Label(card, text="", fg=DANGER, bg=SURFACE, font=font(9), anchor="w")

        tk.Label(self.body, text="or", fg=MUTED, bg=BG, font=font(10)).pack(pady=10)

        # Option 2: Microsoft account
        ms = tk.Frame(self.body, bg=BG)
        ms.pack()
        MinecraftButton(ms, "Sign in with Microsoft", self._start_microsoft_login,
                        microsoft_logo=True, bg=BG).pack()
        self.auth_status = tk.Label(self.body, text="Use the account that owns Minecraft.",
                                    fg=MUTED, bg=BG, font=font(9))
        self.auth_status.pack(pady=(8, 0))

    def _leave_account(self) -> None:
        self._cancel_microsoft_login()
        (getattr(self, "return_to", None) or self.show_library)()

    def _set_account(self, account: Account | None) -> None:
        self.account = account
        if account:
            self.settings["account"] = account.to_settings()
        else:
            self.settings.pop("account", None)
        self._save_settings()
        self._refresh_account_chip()

    def _save_player_name(self) -> None:
        try:
            account = offline_account(self.name_var.get().strip())
        except AuthError as err:
            self.name_error.configure(text=str(err))
            self.name_error.pack(fill="x", pady=(6, 0))
            return
        self._set_account(account)
        self._leave_account()

    def _sign_out(self) -> None:
        self._set_account(None)
        self.show_account()

    def _client_id(self) -> str:
        return os.environ.get("FORESIGHT_MS_CLIENT_ID") or self.settings.get("ms_client_id", "")

    def _start_microsoft_login(self) -> None:
        if self.auth_worker and self.auth_worker.is_alive():
            return
        try:
            auth = MicrosoftAuth(self._client_id())
        except AuthError as err:
            self.auth_status.configure(text=str(err), fg=DANGER)
            return
        self.auth_status.configure(text="Contacting Microsoft…", fg=MUTED)
        self.auth_cancel.clear()
        self.device_code = None
        self.auth_worker = threading.Thread(target=self._auth_worker, args=(auth,), daemon=True)
        self.auth_worker.start()

    def _auth_worker(self, auth: MicrosoftAuth) -> None:
        try:
            code = auth.start()
            self.events.put(("auth_code", code))
            self.events.put(("auth_done", auth.wait(code, self.auth_cancel)))
        except AuthCancelled:
            self.events.put(("auth_cancelled", None))
        except AuthError as err:
            self.events.put(("auth_error", str(err)))
        except Exception as err:  # unexpected response shape, etc.
            self.events.put(("auth_error", f"Sign-in failed: {err}"))

    def _cancel_microsoft_login(self) -> None:
        self.auth_cancel.set()
        self.device_code = None

    def _show_device_code(self, code: DeviceCode) -> None:
        outer, card = self._card(self.body)
        outer.pack(fill="x")
        tk.Label(card, text="Sign in with Microsoft", fg=TEXT, bg=SURFACE, font=font(12, "bold"),
                 anchor="w").pack(fill="x")
        tk.Label(card, text=f"1. Open {code.verification_uri}\n2. Enter this code and approve:",
                 fg=MUTED, bg=SURFACE, font=font(10), anchor="w", justify="left").pack(fill="x",
                                                                                     pady=(4, 10))
        tk.Label(card, text=code.user_code, fg=ACCENT, bg=SURFACE_HI, font=(MONO, 24, "bold"),
                 pady=8).pack(fill="x")
        row = tk.Frame(card, bg=SURFACE)
        row.pack(fill="x", pady=(14, 0))
        Button(row, "Open page", lambda: webbrowser.open(code.verification_uri),
               width=11).pack(side="left")
        Button(row, "Copy code", lambda: self._copy(code.user_code), primary=False,
               width=11).pack(side="left", padx=10)
        Button(row, "Cancel", self._on_cancel_login, primary=False, width=9).pack(side="right")
        self.auth_status = tk.Label(self.body, text="Waiting for you to approve in the browser…",
                                    fg=MUTED, bg=BG, font=font(9))
        self.auth_status.pack(pady=(10, 0))

    def _on_cancel_login(self) -> None:
        self._cancel_microsoft_login()
        self.show_account()

    def _copy(self, text: str) -> None:
        self.root.clipboard_clear()
        self.root.clipboard_append(text)
        self.auth_status.configure(text="Code copied.", fg=MUTED)

    def _handle_auth_event(self, kind: str, payload) -> None:
        on_account = self.current_screen == self.show_account
        if kind == "auth_code":
            if self.auth_cancel.is_set():
                return
            self.device_code = payload
            if on_account:
                self.show_account()
            webbrowser.open(payload.verification_uri)
        elif kind == "auth_done":
            self.device_code = None
            self._set_account(payload)
            if on_account:
                self._leave_account()
        elif kind == "auth_cancelled":
            self.device_code = None
        elif kind == "auth_error":
            self.device_code = None
            if on_account:
                self.show_account()
                self.auth_status.configure(text=str(payload), fg=DANGER)

    # -- worker -> UI -------------------------------------------------------

    def _drain_events(self) -> None:
        try:
            while True:
                kind, payload = self.events.get_nowait()
                self._handle_event(kind, payload)
        except queue.Empty:
            pass
        self.root.after(50, self._drain_events)

    def _handle_event(self, kind: str, payload) -> None:
        if kind.startswith("auth_"):
            self._handle_auth_event(kind, payload)
            return
        if not hasattr(self, "status") or not self.status.winfo_exists():
            return
        if kind == "progress":
            p: Progress = payload
            self.progress.configure(value=int(p.fraction * 1000))
            if p.total_files:
                mb_done = max(p.done_bytes, 0) / 1_048_576
                mb_total = p.total_bytes / 1_048_576
                text = (f"{p.stage}  ·  {p.done_files:,} / {p.total_files:,} files  ·  "
                        f"{mb_done:,.0f} / {mb_total:,.0f} MB")
            else:
                text = f"{p.stage}…"
            self.status.configure(text=text, fg=MUTED)
        elif kind == "done":
            self._set_busy(False)
            self._show_installed()
        elif kind == "cancelled":
            self._set_busy(False)
            self.status.configure(text="Download cancelled. Press Load to resume.", fg=MUTED)
        elif kind == "error":
            self._set_busy(False)
            self.status.configure(text=f"Download failed: {payload}", fg=DANGER)

    # -- misc ---------------------------------------------------------------

    def _open_folder(self) -> None:
        path: Path = self.installer.install_dir
        path.mkdir(parents=True, exist_ok=True)
        if sys.platform == "win32":
            os.startfile(path)  # type: ignore[attr-defined]
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(path)])
        else:
            subprocess.Popen(["xdg-open", str(path)])

    def _on_close(self) -> None:
        self.installer.cancel()
        self.auth_cancel.set()
        self.root.destroy()


def run() -> None:
    root = tk.Tk()
    ForesightApp(root)
    root.mainloop()
