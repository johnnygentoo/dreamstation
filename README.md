# DreamStation

A Sega Dreamcast, Naomi and Atomiswave emulator for Linux, with a native GTK 4
interface. The emulation itself is [Flycast](https://github.com/flyinghead/flycast)'s;
DreamStation wraps it in a desktop app that tries to feel like a proper
front end rather than a settings menu.

## What it does

- **Game library**: your folders are scanned for GDI, CDI, CHD and CUE images and
  for Naomi / Naomi 2 / Atomiswave ROM sets (zipped or unzipped). Covers are
  fetched automatically, shown flat or as 3D boxes, and the list refreshes itself
  when you add games.
- **Per-game settings**, save states with screenshots, resume on start, play time.
- **VMUs**: a VMU manager for your memory cards, the VMU screen as an overlay or
  in its own window, and VMU mini-games (Chao Adventure and friends) playable on
  the desktop when a game hands them over.
- **Shaders**: a post-processing chain that takes Dolphin/DuckStation-style
  `.glsl` shaders, RetroArch slang presets and ReShade `.fx` effects.
- **Online play** with a friend (rollback netplay), and the Dreamcast's own
  online options for games with community servers.
- **Controllers** with per-port bindings, arcade sticks, rumble, and the arcade
  cabinet's test, service and coin switches.

## Before you start: BIOS and games

DreamStation does not include any BIOS, firmware or games, and this project
can't help you find them. Use dumps of hardware and discs you own.

| File | What it is | Where it goes |
|---|---|---|
| `dc_boot.bin` | Dreamcast boot ROM | Settings → BIOS → Boot ROM |
| `dc_flash.bin` | Dreamcast flash (settings memory) | Settings → BIOS → Flash ROM |
| `naomi.zip`, `naomi2.zip`, `awbios.zip`, … | arcade board BIOS | the same folder as `dc_boot.bin` |

Then add the folders that hold your games under **Settings → Game List**.

## Running it

Download the AppImage from the [Releases](https://github.com/Johnnygentoo/dreamstation/releases)
page, make it executable and run it:

```sh
chmod +x DreamStation-*-x86_64.AppImage
./DreamStation-*-x86_64.AppImage
```

You need a GPU with OpenGL 4.5, and a distribution with glibc 2.44 or newer
(`ldd --version` tells you). On older systems, build from source.

## Building from source

You'll need a C++23 compiler (GCC 13+ or Clang 17+), CMake 3.25+, Ninja or Make,
and the development files for:

- GTK 4 (4.10+) and gtkmm 4
- libadwaita (1.5+) and **libadwaitamm** (see below)
- SDL 3
- libepoxy
- zlib

spdlog, toml++ and Catch2 are downloaded by CMake during the first configure.

On Arch, for example:

```sh
sudo pacman -S --needed base-devel cmake ninja git gtkmm-4.0 libadwaita sdl3 libepoxy zlib meson mm-common perl-xml-parser
```

Few distributions package [libadwaitamm](https://github.com/rofirrim/libadwaitamm)
yet. If yours doesn't, build it first (this is the revision DreamStation is
tested with):

```sh
git clone https://github.com/rofirrim/libadwaitamm.git
cd libadwaitamm
git checkout 326ea0f
meson setup build --prefix=/usr/local -Dbuild-documentation=false -Dbuild-examples=false
meson compile -C build
sudo meson install -C build
```

Then DreamStation:

```sh
git clone --recursive https://github.com/Johnnygentoo/dreamstation.git
cd dreamstation
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bin/dreamstation
```

If libadwaitamm went to `/usr/local`, point CMake at it with
`PKG_CONFIG_PATH=/usr/local/lib/pkgconfig` (or `lib64`) before `cmake`.

To install system-wide: `sudo cmake --install build`. The included `Makefile`
wraps the same steps (`make`, then `sudo make install`).

### Building the AppImage

`packaging/appimage/build-appimage.sh` builds a release AppImage from a clean
checkout. See the comments at the top of the script for what it needs.

## Where things are kept

| What | Where |
|---|---|
| Settings | `~/.config/dreamstation/` |
| Saves, VMUs, save states, shaders | `~/.local/share/dreamstation/` |
| Cover thumbnails | `~/.cache/dreamstation/` |

ReShade shader packs go in `~/.local/share/dreamstation/shaders/reshade/`.

## Credits

DreamStation would be nothing without **Flycast** by flyinghead and its
contributors, and the reicast and nullDC work it grew from. ReShade's effect
compiler is by Patrick Mours. The interface owes a lot to DuckStation. The full
list, with licences, is in [CREDITS.md](CREDITS.md).

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE). Flycast is used under the same terms
(it is GPL-2.0-or-later).

Sega, Dreamcast and Naomi are trademarks of Sega; Atomiswave is a trademark of
Sammy. This project is not affiliated with either.
