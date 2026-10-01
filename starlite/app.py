"""Starlite launcher UI (tkinter, standard library only).

Screens:
  1. Library  - the Minecraft game card with Activate / Load.
  2. Versions - Minecraft 1.21.11 with a Load button that downloads every file.
"""

from __future__ import annotations

import json
import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import ttk

from .minecraft import DownloadCancelled, MinecraftInstaller, Progress

APP_NAME = "Starlite"
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


class StarliteApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.installer = MinecraftInstaller()
        self.settings_path = self.installer.install_dir / "launcher.json"
        self.settings = self._load_settings()
        self.events: "queue.Queue[tuple]" = queue.Queue()
        self.worker: threading.Thread | None = None

        root.title(APP_NAME)
        root.geometry("760x480")
        root.minsize(640, 420)
        root.configure(bg=BG)
        root.protocol("WM_DELETE_WINDOW", self._on_close)

        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure("Starlite.Horizontal.TProgressbar", troughcolor=SURFACE_HI,
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
        except OSError:
            pass

    @property
    def activated(self) -> bool:
        return bool(self.settings.get("minecraft_activated"))

    # -- layout helpers -----------------------------------------------------

    def _build_header(self) -> None:
        header = tk.Frame(self.root, bg=BG)
        header.pack(fill="x", padx=32, pady=(24, 8))
        tk.Label(header, text="●", fg=ACCENT, bg=BG, font=font(12)).pack(side="left")
        tk.Label(header, text=f" {APP_NAME.upper()}", fg=TEXT, bg=BG,
                 font=font(13, "bold")).pack(side="left")
        tk.Label(header, text="Launcher", fg=MUTED, bg=BG, font=font(11)).pack(side="left", padx=8)

    def _clear(self) -> None:
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

        self.progress = ttk.Progressbar(card, style="Starlite.Horizontal.TProgressbar",
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
        self.root.destroy()


def run() -> None:
    root = tk.Tk()
    StarliteApp(root)
    root.mainloop()
