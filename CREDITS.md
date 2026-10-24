# Credits and licences

DreamStation is free software under the **GNU General Public License, version 3
or later** (see [LICENSE](LICENSE)). It stands on the work of many people; this
file says whose, and under which terms.

## The emulation engine: Flycast

All of the actual Dreamcast, Naomi and Atomiswave emulation (CPU, graphics,
sound, GD-ROM, Maple bus, VMUs, netplay) is done by
**[Flycast](https://github.com/flyinghead/flycast)**, by flyinghead and its
contributors, which itself grew out of **reicast** and **nullDC**.

- Licence: GNU GPL version 2 or (at your option) any later version. DreamStation
  uses it under version 3.
- DreamStation builds a fork of Flycast as a library:
  [Johnnygentoo/flycast](https://github.com/Johnnygentoo/flycast), branch
  `dreamstation`, included here as the `engine/flycast` submodule. The fork's
  commits on top of upstream are listed in its history; in short they add a
  static library build, unzipped Naomi ROM-set folders, VMU screen and
  unplugging hooks, arcade input helpers, and netplay in the library build, and
  fix the checksum Flycast's netplay uses for CDI/GDI/CUE images.
- Flycast includes further libraries under their own licences (libchdr, zstd,
  libzip, glm, xbyak, miniupnpc, GGPO, libretro-common and others); see
  `engine/flycast/core/deps` and the licence files there.

If DreamStation is useful to you, Flycast is the reason it works. Please
consider supporting the Flycast project.

## Included in this repository

| Component | Author | Licence | Where |
|---|---|---|---|
| ReShade FX effect compiler (preprocessor, parser, GLSL back end), v6.8.0, unmodified | Patrick Mours | BSD-3-Clause | `third_party/reshadefx` (licence in that folder) |

## Fetched at build time

| Component | Licence |
|---|---|
| [spdlog](https://github.com/gabime/spdlog) | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT |
| [Catch2](https://github.com/catchorg/Catch2) (tests only, not shipped) | BSL-1.0 |

## Linked system libraries

These are linked dynamically. The AppImage carries copies of them; their
sources are available from their projects.

| Component | Licence |
|---|---|
| GTK 4, glibmm, gtkmm, libadwaita, libadwaitamm | LGPL-2.1-or-later |
| SDL 3 | zlib |
| libepoxy | MIT |

## Data used at run time (not included)

- **Cover art** is downloaded on request from the
  [libretro thumbnail project](https://github.com/libretro-thumbnails); the
  images belong to their respective owners.
- **ReShade shaders** are not included; you add shader packs yourself, under
  their own licences.

## Inspiration

The interface follows the layout and many ideas of
[DuckStation](https://www.duckstation.org) by Stenzek. No DuckStation code is
used.

## Trademarks

Sega, Dreamcast, Naomi and VMU are trademarks of Sega Corporation. Atomiswave is
a trademark of Sammy Corporation. DreamStation is not affiliated with, endorsed
by or sponsored by either company. No BIOS, firmware or game is included with
DreamStation.
