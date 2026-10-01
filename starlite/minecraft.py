"""Download everything a Minecraft: Java Edition client version needs.

All files come from Mojang's official servers and are verified by SHA-1:

* version JSON + client jar   -> versions/<id>/
* libraries (incl. natives)   -> libraries/
* asset index + asset objects -> assets/indexes/, assets/objects/
* logging config              -> assets/log_configs/

The on-disk layout matches the vanilla ``.minecraft`` directory.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import sys
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

VERSION_MANIFEST_URL = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
ASSET_BASE_URL = "https://resources.download.minecraft.net"
USER_AGENT = "Starlite-Launcher/1.0"

CHUNK_SIZE = 64 * 1024
RETRIES = 3
WORKERS = 8


class DownloadCancelled(Exception):
    """Raised when the user cancels an install."""


class DownloadError(Exception):
    """Raised when a file can't be fetched or fails verification."""


@dataclass(frozen=True)
class FileTask:
    url: str
    path: Path
    sha1: Optional[str]
    size: int


@dataclass
class Progress:
    stage: str
    done_files: int = 0
    total_files: int = 0
    done_bytes: int = 0
    total_bytes: int = 0

    @property
    def fraction(self) -> float:
        if self.total_bytes:
            return min(self.done_bytes / self.total_bytes, 1.0)
        if self.total_files:
            return min(self.done_files / self.total_files, 1.0)
        return 0.0


ProgressCallback = Callable[[Progress], None]


def default_install_dir() -> Path:
    """Per-user data directory, kept separate from the official ``.minecraft``."""
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming"))
        return base / ".starlite"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "starlite"
    return Path.home() / ".starlite"


def current_os_name() -> str:
    """Mojang's OS names: ``windows``, ``osx`` or ``linux``."""
    if sys.platform == "win32":
        return "windows"
    if sys.platform == "darwin":
        return "osx"
    return "linux"


def current_arch() -> str:
    machine = platform.machine().lower()
    if machine in ("arm64", "aarch64"):
        return "arm64"
    if machine in ("x86", "i386", "i686"):
        return "x86"
    return "x86_64"


def rules_allow(rules: Optional[list], os_name: str, arch: str) -> bool:
    """Evaluate a library's ``rules`` list; the last matching rule wins."""
    if not rules:
        return True
    allowed = False
    for rule in rules:
        if "features" in rule:
            # Feature rules (demo mode, custom resolution...) never apply to downloads.
            continue
        os_rule = rule.get("os", {})
        if "name" in os_rule and os_rule["name"] != os_name:
            continue
        if "arch" in os_rule and os_rule["arch"] != arch:
            continue
        allowed = rule.get("action") == "allow"
    return allowed


def sha1_of(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(CHUNK_SIZE), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_valid(task: FileTask) -> bool:
    if not task.path.is_file():
        return False
    if task.size and task.path.stat().st_size != task.size:
        return False
    return task.sha1 is None or sha1_of(task.path) == task.sha1


def _open(url: str):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    return urllib.request.urlopen(request, timeout=30)


def fetch_json(url: str) -> dict:
    with _open(url) as response:
        return json.load(response)


class MinecraftInstaller:
    """Resolves and downloads every file for one Minecraft version."""

    def __init__(self, install_dir: Optional[Path] = None, workers: int = WORKERS):
        self.install_dir = Path(install_dir or default_install_dir())
        self.workers = workers
        self.os_name = current_os_name()
        self.arch = current_arch()
        self._cancel = threading.Event()

    # -- public API ---------------------------------------------------------

    def cancel(self) -> None:
        self._cancel.set()

    def version_dir(self, version_id: str) -> Path:
        return self.install_dir / "versions" / version_id

    def is_installed(self, version_id: str) -> bool:
        """Cheap check: the marker is written only after a fully verified install."""
        return (self.version_dir(version_id) / ".installed").is_file()

    def install(self, version_id: str, on_progress: Optional[ProgressCallback] = None) -> Path:
        """Download and verify all files for ``version_id``; returns the version dir."""
        self._cancel.clear()
        report = on_progress or (lambda _p: None)

        report(Progress("Fetching version list"))
        version_entry = self._find_version(version_id)

        report(Progress(f"Fetching {version_id} metadata"))
        version_json_path = self.version_dir(version_id) / f"{version_id}.json"
        self._download_all(
            [FileTask(version_entry["url"], version_json_path, version_entry.get("sha1"), 0)],
            lambda _p: None,
            stage="",
        )
        version = json.loads(version_json_path.read_text(encoding="utf-8"))

        report(Progress("Fetching asset index"))
        asset_index = version["assetIndex"]
        asset_index_path = self.install_dir / "assets" / "indexes" / f"{asset_index['id']}.json"
        self._download_all(
            [FileTask(asset_index["url"], asset_index_path, asset_index["sha1"], asset_index["size"])],
            lambda _p: None,
            stage="",
        )
        assets = json.loads(asset_index_path.read_text(encoding="utf-8"))

        tasks = self.collect_tasks(version, assets)
        self._download_all(tasks, report, stage=f"Downloading Minecraft {version_id}")

        (self.version_dir(version_id) / ".installed").write_text(
            time.strftime("%Y-%m-%dT%H:%M:%S"), encoding="utf-8"
        )
        total = Progress("Done", len(tasks), len(tasks))
        report(total)
        return self.version_dir(version_id)

    # -- task resolution ----------------------------------------------------

    def collect_tasks(self, version: dict, assets: dict) -> list[FileTask]:
        """Every file the client needs, de-duplicated by destination path."""
        version_id = version["id"]
        tasks: dict[Path, FileTask] = {}

        def add(task: FileTask) -> None:
            tasks.setdefault(task.path, task)

        client = version["downloads"]["client"]
        add(FileTask(client["url"], self.version_dir(version_id) / f"{version_id}.jar",
                     client["sha1"], client["size"]))

        for task in self._library_tasks(version.get("libraries", [])):
            add(task)

        for name, obj in sorted(assets.get("objects", {}).items()):
            digest = obj["hash"]
            add(FileTask(
                f"{ASSET_BASE_URL}/{digest[:2]}/{digest}",
                self.install_dir / "assets" / "objects" / digest[:2] / digest,
                digest,
                obj["size"],
            ))

        log_file = version.get("logging", {}).get("client", {}).get("file")
        if log_file:
            add(FileTask(log_file["url"],
                         self.install_dir / "assets" / "log_configs" / log_file["id"],
                         log_file["sha1"], log_file["size"]))

        return list(tasks.values())

    def _library_tasks(self, libraries: list) -> list[FileTask]:
        lib_root = self.install_dir / "libraries"
        out = []
        for lib in libraries:
            if not rules_allow(lib.get("rules"), self.os_name, self.arch):
                continue
            downloads = lib.get("downloads", {})

            artifact = downloads.get("artifact")
            if artifact and artifact.get("url"):
                out.append(FileTask(artifact["url"], lib_root / artifact["path"],
                                    artifact.get("sha1"), artifact.get("size", 0)))

            # Older versions ship natives as classifiers instead of separate libraries.
            natives_key = lib.get("natives", {}).get(self.os_name)
            if natives_key:
                arch_bits = "64" if self.arch in ("x86_64", "arm64") else "32"
                classifier = downloads.get("classifiers", {}).get(
                    natives_key.replace("${arch}", arch_bits))
                if classifier:
                    out.append(FileTask(classifier["url"], lib_root / classifier["path"],
                                        classifier.get("sha1"), classifier.get("size", 0)))
        return out

    def _find_version(self, version_id: str) -> dict:
        manifest = fetch_json(VERSION_MANIFEST_URL)
        for entry in manifest["versions"]:
            if entry["id"] == version_id:
                return entry
        raise DownloadError(f"Minecraft version {version_id} was not found in Mojang's manifest.")

    # -- downloading --------------------------------------------------------

    def _download_all(self, tasks: list[FileTask], report: ProgressCallback, stage: str) -> None:
        progress = Progress(stage, total_files=len(tasks),
                            total_bytes=sum(t.size for t in tasks))
        lock = threading.Lock()
        last_report = [0.0]

        def advance(files: int = 0, nbytes: int = 0, force: bool = False) -> None:
            with lock:
                progress.done_files += files
                progress.done_bytes += nbytes
                now = time.monotonic()
                if not force and now - last_report[0] < 0.05:
                    return
                last_report[0] = now
                snapshot = Progress(**vars(progress))
            report(snapshot)

        advance(force=True)
        with ThreadPoolExecutor(max_workers=self.workers) as pool:
            futures = [pool.submit(self._download_one, task, advance) for task in tasks]
            try:
                for future in as_completed(futures):
                    future.result()
            except BaseException:
                self._cancel.set()
                for future in futures:
                    future.cancel()
                raise
        advance(force=True)

    def _download_one(self, task: FileTask, advance: Callable[..., None]) -> None:
        if self._cancel.is_set():
            raise DownloadCancelled()
        if is_valid(task):
            advance(files=1, nbytes=task.size)
            return

        task.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = task.path.with_name(task.path.name + ".part")
        last_error: Optional[Exception] = None

        for attempt in range(1, RETRIES + 1):
            received = 0
            try:
                digest = hashlib.sha1()
                with _open(task.url) as response, tmp.open("wb") as fh:
                    while True:
                        if self._cancel.is_set():
                            raise DownloadCancelled()
                        chunk = response.read(CHUNK_SIZE)
                        if not chunk:
                            break
                        fh.write(chunk)
                        digest.update(chunk)
                        received += len(chunk)
                        advance(nbytes=len(chunk))
                if task.sha1 and digest.hexdigest() != task.sha1:
                    raise DownloadError(f"Checksum mismatch for {task.path.name}")
                os.replace(tmp, task.path)
                advance(files=1)
                return
            except DownloadCancelled:
                tmp.unlink(missing_ok=True)
                raise
            except Exception as exc:  # network hiccup or bad checksum: retry
                last_error = exc
                advance(nbytes=-received)
                tmp.unlink(missing_ok=True)
                if attempt < RETRIES:
                    time.sleep(attempt)

        raise DownloadError(f"Failed to download {task.url}: {last_error}")
