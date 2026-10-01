# Starlite

A simple, minimal Minecraft: Java Edition launcher.

| Library | Version |
|---|---|
| ![Library](docs/library.png) | ![Downloading 1.21.11](docs/download.png) |

## How it works

1. **Library**: the Minecraft card. Press **Activate**, then **Load**.
2. **Version**: shows **1.21.11**. Press **Load** to download every file the
   game needs. You can cancel at any time and pressing Load again resumes.

What gets downloaded (all from Mojang's official servers, every file
SHA-1 verified):

| What | Where |
|---|---|
| Version JSON + client jar | `versions/1.21.11/` |
| Libraries (incl. natives for your OS) | `libraries/` |
| Asset index + ~4,600 asset objects | `assets/indexes/`, `assets/objects/` |
| Logging config | `assets/log_configs/` |

About 530 MB in total. The layout matches the vanilla `.minecraft` folder.
Files that are already present and valid are skipped, so re-running only
re-verifies.

Install location (separate from your official `.minecraft`):

| OS | Path |
|---|---|
| Windows | `%APPDATA%\.starlite` |
| macOS | `~/Library/Application Support/starlite` |
| Linux | `~/.starlite` |

## Run

Requires Python 3.10+ and nothing else (standard library only, UI is tkinter).

```
python -m starlite
```

Terminal-only download:

```
python -m starlite --no-gui --version 1.21.11 --dir ./mc
```

Tests:

```
python -m unittest discover -s tests
```

> On Linux, tkinter may need `sudo apt install python3-tk`.

## Project layout

| Path | Purpose |
|---|---|
| `starlite/minecraft.py` | Download engine: manifest, libraries, assets, parallel downloads, checksums, retries. |
| `starlite/app.py` | Launcher UI (Library → Version screens). |
| `starlite/__main__.py` | Entry point and `--no-gui` mode. |
| `tests/` | Unit tests for rule evaluation and file resolution. |

## Not yet included

* Starting the game (needs a Java 21 runtime and Microsoft account sign-in).
* Other versions; the engine supports any version ID, the UI shows 1.21.11.
