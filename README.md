[![License: BSD 2-Clause](https://img.shields.io/badge/License-BSD_2--Clause-blue.svg)](https://opensource.org/licenses/BSD-2-Clause)

# openwog2

*An unofficial, non-commercial open-source game engine, written entirely with free-tier LLM coding agents and websites.*

<img width="320" height="200" alt="screenshot_20261104_233805" src="https://github.com/user-attachments/assets/8235df9c-558f-4bd3-9c1c-fb1cf75f7b3e" />
<img width="320" height="200" alt="screenshot_20261104_233834" src="https://github.com/user-attachments/assets/60be2c8e-9f87-416b-8f4a-449485377d18" />
<img width="320" height="200" alt="screenshot_20261104_233909" src="https://github.com/user-attachments/assets/dfdf80d2-a1a7-462c-bf00-2fb7a8185abb" />
<img width="320" height="200" alt="screenshot_20261104_233939" src="https://github.com/user-attachments/assets/e1c99fdd-a592-41e8-940c-5b53144a2e12" />
<img width="320" height="200" alt="screenshot_20261104_234126" src="https://github.com/user-attachments/assets/ebeb316f-e4a8-40d5-b299-a591558bc08c" />
<img width="320" height="200" alt="screenshot_20261104_234624" src="https://github.com/user-attachments/assets/c04e858c-7fed-4e02-8ab0-33f695e5011c" />

**openwog2** is an open-source C++/SDL2 reimplementation of the game engine of the classic Korean MS-DOS SRPG **The War of Genesis II (창세기전 2)**, made for modern-platform compatibility and preservation. It is an engine only and cannot run without the original game files.

## Built with Free AI Tools

All code in this project was written solely with LLM coding agents and websites available on free tiers; no paid plans or API credits were used.

## Status

* **Playable, but in early development.** Only the early part of the game has been tested.
* **BGM (CD audio) is not implemented yet.**

## Build

Requirements: C++20 compiler, CMake 3.20+, SDL2, SDL2_mixer.

### Linux
```bash
# sudo apt-get install build-essential cmake libsdl2-dev libsdl2-mixer-dev

cmake -B build -S .
cmake --build build
```

### Windows (Command Prompt for VS)
```cmd
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## How to Play

1. Install the game from your original CD, then apply the 1.2 patch.
2. Copy the contents of the installation folder into a `GENESIS2` folder next to the built executable.
3. From the CD, copy `HICOM.FLI`, `SOFTMAX.FLI`, and the entire `CHAPDATA` and `SCENARIO` folders into the same `GENESIS2` folder.
4. Run the executable.

## Third-Party Libraries

Uses [SDL2](https://www.libsdl.org/) and [SDL2_mixer](https://github.com/libsdl-org/SDL_mixer). 

## Legal Notice

* This repository (and any binary release) does not include the original executables, data files, graphics, music or sound effects.
* *The War of Genesis* (창세기전) and *The War of Genesis II* (창세기전 2), including their names, characters, story, artwork, and music, belong to their rights holders: Softmax Co., Ltd. (original developer/publisher, 1996) and LINE Games Corporation or its successors/licensors. This project is **not affiliated with, authorized, sponsored, or endorsed by** any of them.
* Names are used only to indicate which game this engine is compatible with. No official logos are used and no ownership is claimed. If a rights holder requests a rename, we will respond promptly.
* You are responsible for ensuring that your acquisition and use of the game data and of this software comply with the laws of your country and the terms of your copy of the game. Nothing here is legal advice.
* The software is provided "as is", without warranty of any kind (see [LICENSE](LICENSE)).

## License

The source code of openwog2 is licensed under the [BSD 2-Clause License](LICENSE).

This license covers **only** the source code written for this project. It grants no rights to the original game, its assets, or its trademarks.
