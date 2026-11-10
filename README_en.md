# **Raindrop**

An enhancement patch adding features such as stage selection for *Samidare* Ver. 1.054 and *RefRain - prism memories -* Ver. 1.03.7.

## Usage

### Samidare

Download and apply the update patch from the [official site](https://www.rebrank.org/download.htm), then download `ddraw.dll` from [Releases](https://github.com/Madotsukanai/Raindrop/releases/latest), copy it to the same directory as `Rain.exe` (SHA-256: `ae35b0ad17094bd21f86bc464504ceeafc50941e5207d75a3065e44049138211`), and start the game.

Choose the graphics API in `raindrop.ini` in the same directory as `Rain.exe`:

```ini
[General]
PrEx=1
[Graphics]
API=DirectDraw
```

`API=DirectDraw` uses the system DirectDraw implementation and falls back to D3D9 if it is unavailable. Set `API=D3D9` to use the built-in D3D9 implementation from startup. If the file or setting is missing, `DirectDraw` is used.<br>
On Wine/Proton, keeping the default `DirectDraw` is recommended.

### RefRain

Too broke to own the Steam version, so only the disc-version executable is supported.

Download `d3d9.dll` from [Releases](https://github.com/Madotsukanai/Raindrop/releases/latest), copy it to the same directory as `RefRain.exe` (SHA-256: `db092baa547241a0217ca41be8538672b79094f671884f92a32c06943c857e78`), and start the game.

To open the Practice menu, move the cursor to “DIVE 2 M.R.S.” and press the key assigned to Concept Reactor.

## Building
### Windows
Use `clang-cl` from an x86 Native Tools Command Prompt. Add the LLVM/Clang tools in Visual Studio Installer, then run this from the repository root. `NMakefile` uses the MSVC Windows SDK, `link.exe`, and libraries to produce x86 DLLs. The bundled D3DX9 compatibility declarations mean the legacy DirectX SDK is not required:

```bat
nmake /f NMakefile smd            :: Samidare: build\ddraw.dll
nmake /f NMakefile smd-debug      :: Samidare debug: build\debug\ddraw.dll (SMD_DEBUG_BUILD, logging enabled)
nmake /f NMakefile refrain        :: RefRain: build\d3d9.dll
nmake /f NMakefile refrain-debug  :: RefRain debug: build\debug\d3d9.dll (REFRAIN_DEBUG_BUILD, logging enabled)
nmake /f NMakefile debug          :: all debug builds (refrain-debug + smd-debug)
nmake /f NMakefile                :: refrain + smd
```

## Building on Linux

Cross-compile with MinGW-w64 (32-bit). Install `i686-w64-mingw32-gcc`, then run `make` from the repository root:

```sh
# Arch / CachyOS
sudo pacman -S mingw-w64-gcc

# Debian / Ubuntu
sudo apt install mingw-w64
```

```sh
make smd            # Samidare: build/ddraw.dll
make smd-debug      # Samidare debug: build/debug/ddraw.dll (SMD_DEBUG_BUILD, logging enabled)
make refrain        # RefRain: build/d3d9.dll
make refrain-debug  # RefRain debug: build/debug/d3d9.dll (REFRAIN_DEBUG_BUILD, logging enabled)
make debug          # all debug builds (refrain-debug + smd-debug)
make all            # refrain + smd
```

## Features

- Reduced input latency

### Samidare

- Windows 11 support
- Extra stage added to the Practice menu

![Practice](https://madotsukanai.github.io/raindrop2.png)
After clearing all stages, select Extra in Practice and use the left and right keys to cycle through Area 1/4–4/4, Boss 1/2, and Boss 2/2. Practice starts from the selected area or boss fight.

- Bug fixes

Fixes known game bugs, including scores carrying over after retrying from the results screen, some sound effects such as the enemy bullet warning not playing, and an invisible option in the replay menu.

- Replay desynchronization fixes in Wine/Proton environments

Forces bullet and player coordinates in replays to use 24-bit floating-point calculations.

- Keyboard shortcuts
  - **D**: Restore the default settings on the settings screen.
  - **Q**: Return to the title screen from the pause menu.
  - **R**: Retry from the pause menu.
  - **Alt+Enter**: Switch between windowed and fullscreen modes.

- Pause BGM and sound effects while the pause menu is open
- Enable pausing during replay playback
- Adjust starting power in Practice mode

Stage 2 starts with power level 6, and Stage 3 starts at maximum power.

### RefRain

- Keyboard shortcuts
  - **Q**: Return to the title screen from the pause menu.
  - **R**: Retry from the pause menu.

- Practice mode

Adjust the stage, lives, M.E.F.A.2 gauge, Concept Reactor stock, and Concept Reactor gauge. When a stage ends, the game returns to that character's desktop screen. During play, press **F1–F5** to jump directly to the corresponding stage (score, prisms, and other progress are reset).

![Stage select](https://madotsukanai.github.io/raindrop.png)

## Notes

- This is an unofficial tool and is not affiliated with RebRank. **Please do not contact RebRank or its associates about this software.**
- Back up your save data before using unofficial tools.
- **Replays recorded with this tool cannot be submitted to the official scoreboard.**
