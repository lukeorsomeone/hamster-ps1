# Rodent Cage - PS1 Edition

A PlayStation 1 port of the cage part of my Godot rodent pet game, written in C with
[PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK).

Hamster (HAMMY), guinea pig (POPCORN), chinchilla (MISTY) and sugar glider (KIWI), each
in 3 colours, living in the room from the Godot game. Feed, water, pet, give treats, clean
up, day/night cycle.

## Controls

| Button | Action |
|---|---|
| X | Fill the food bowl |
| O | Refill the water |
| Square | Pet |
| L2 | Give a treat |
| R2 | Spot clean |
| Triangle | Zoom on the pet |
| L1 / R1 | Turn camera |
| D-pad up/down | Tilt camera |
| Select | Change colour |
| Start | Pause menu (stats, species, colour) |

## Setting up a PC to build it (Windows)

1. Install **CMake** (cmake.org, tick "Add to PATH") and Ninja: `winget install Ninja-build.Ninja`
2. Download the **PSn00bSDK** release ZIP (`PSn00bSDK-...-win32.zip`, not the gcc one) and
   extract it so `C:\PSn00bSDK\bin` exists.
3. In PowerShell:
   ```powershell
   [Environment]::SetEnvironmentVariable("Path", [Environment]::GetEnvironmentVariable("Path","User") + ";C:\PSn00bSDK\bin", "User")
   [Environment]::SetEnvironmentVariable("PSN00BSDK_LIBS", "C:\PSn00bSDK\lib\libpsn00b", "User")
   ```
   then open a new PowerShell window.

## Building

```powershell
cmake --preset default
cmake --build ./build
```

Then open `build/hamster.cue` in DuckStation (needs your own PS1 BIOS).

## Layout

- `src/game.c` - the whole game: scene, pets, AI, HUD (no hardware calls)
- `src/main.c` - PS1 layer: GPU, GTE, controller, font
- `src/plat.h` - the interface between the two
- `src/font_psx.h` - the Godot game's font converted to a PS1 texture (`tools/make_font.py`)
- `pctest/` - PC preview: renders frames of the game to PNG without a PS1
  (`gcc -O2 -o pc pctest/pc_main.c src/game.c`, run it from `pctest/`, then `python compose.py`)
