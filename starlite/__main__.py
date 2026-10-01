"""Entry point: ``python -m starlite`` opens the launcher.

``python -m starlite --no-gui [--version 1.21.11] [--dir PATH]`` downloads
from the terminal instead.
"""

import argparse
import sys
from pathlib import Path

from .minecraft import DownloadCancelled, MinecraftInstaller, Progress


def _cli(version: str, install_dir: Path | None) -> int:
    installer = MinecraftInstaller(install_dir)
    print(f"Installing Minecraft {version} into {installer.install_dir}")

    def show(p: Progress) -> None:
        bar = "#" * int(p.fraction * 30)
        sys.stdout.write(f"\r[{bar:<30}] {p.fraction:6.1%}  {p.done_files}/{p.total_files} files  {p.stage[:40]:<40}")
        sys.stdout.flush()

    try:
        path = installer.install(version, show)
    except (KeyboardInterrupt, DownloadCancelled):
        installer.cancel()
        print("\nCancelled.")
        return 130
    except Exception as exc:
        print(f"\nFailed: {exc}")
        return 1
    print(f"\nDone: {path}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(prog="starlite", description="Starlite Minecraft launcher")
    parser.add_argument("--no-gui", action="store_true", help="download from the terminal")
    parser.add_argument("--version", default="1.21.11", help="Minecraft version (with --no-gui)")
    parser.add_argument("--dir", type=Path, default=None, help="install directory (with --no-gui)")
    args = parser.parse_args()

    if args.no_gui:
        return _cli(args.version, args.dir)

    from .app import run
    run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
