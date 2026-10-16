# Scamper

Makes desktop icons slowly wander. Every few seconds Scamper nudges icons a few pixels in random directions, and it can put them all back exactly where they were.

It runs in the notification area by default. Click the tray icon to pause it, to turn "Run at startup" on or off, to restore the icons and exit, or to exit and leave the icons where they are. It works on Windows 7 through Windows 11.

## Usage

```
scamper [-IntervalSeconds 1-3600] [-PixelsPerStep 1-20] [-IconsPerTick 0-2147483647]
        [-FullscreenOnly] [-Console] [-Startup]
scamper -Restore
scamper -RemoveStartup
```

| Flag | What it does |
| --- | --- |
| `-IntervalSeconds` | Seconds between moves (default 15). |
| `-PixelsPerStep` | Maximum pixels an icon moves per tick (default 5). |
| `-IconsPerTick` | How many random icons move per tick; 0 moves all (default 0). |
| `-FullscreenOnly` | Only move icons while the foreground app fills a monitor. |
| `-Console` | Run in the current console instead of the tray (Ctrl+C stops it). |
| `-Startup` | Start in the tray at every sign-in with these options, and start now. |
| `-RemoveStartup` | Stop starting at sign-in. |
| `-Restore` | Stop any running copy and put icons back where they were before the first run. |

Flags work like PowerShell parameters: they're case-insensitive, can be shortened to any unique prefix (`-Interval 30`), and accept `-Name:value`.

On its first run Scamper saves the original layout to `scamper-originalDesktopPosition.json` next to the exe. Each later start first restores that layout and then drifts from there, so the drift doesn't pile up across runs. `-Restore` puts the icons back and deletes the file. Only one copy runs at a time.

## Download

Prebuilt `scamper-x64.exe` and `scamper-x86.exe` (for 32-bit Windows) are attached to each release on the Releases page. Every push is also built by GitHub Actions, and the exes are attached to that workflow run as artifacts.

To publish a release, push a version tag. The workflow builds both exes and creates the release:

```
git tag v1.0.0
git push origin v1.0.0
```

## Building

Build from a Developer PowerShell or Developer Command Prompt for Visual Studio with the "Desktop development with C++" workload:

```
rc /nologo scamper.rc
cl /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE scamper.cpp scamper.res
```

The C runtime is linked statically, so the exe needs no redistributable.

### Windows 7

Build with the VS 2022 (v143 / 14.4x) toolset. Install the "MSVC v143 - VS 2022 C++ x64/x86 build tools" component, then open a dev shell for it from a plain PowerShell window:

```powershell
Import-Module "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "C:\Program Files\Microsoft Visual Studio\18\Community" -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64 -vcvars_ver=14.44"
```

For 32-bit Windows 7, use `-arch=x86`.

## Other files

- `scamper.ps1` is the original PowerShell version. It takes the same core flags and uses the same layout format. The C++ version picks up a layout the script saved (`DesktopDrift-OriginalLayout.json`) and renames it.
- `tools/make-ico.ps1` rebuilds `scamper.ico` (16 to 256 px) from `scamper-icon-raw.png`:

  ```powershell
  .\tools\make-ico.ps1 -Source "$PWD\scamper-icon-raw.png" -Destination "$PWD\scamper.ico"
  ```
