# Building

## Quick start (Ubuntu 24.04 / 22.04, Debian 12)

```bash
git clone <this-repo> keeperrl
cd keeperrl
./build-ubuntu.sh
./keeper
```

`build-ubuntu.sh` installs the dependencies, creates the build directories and compiles.
Expect **10-25 minutes** on a typical machine.

**Run the game from the repository directory.** The game reads `appconfig.txt`,
`data_free/` and `mods/` from the *current working directory*, not from wherever the
binary lives. `./keeper` from the repo root works; `~/keeperrl/keeper` from your home
directory does not. Use `--data_dir /path/to/keeperrl` if you need to run it elsewhere.

---

## Dependencies

Verified on **Ubuntu 24.04.3 LTS** with clang 18.1.3 and GNU Make 4.3.

```bash
sudo apt install clang make git pkg-config \
    libsdl2-dev libsdl2-image-dev libgl1-mesa-dev libopenal-dev \
    libvorbis-dev libogg-dev libtheora-dev \
    zlib1g-dev libcurl4-openssl-dev libssl-dev liblzma-dev
```

| library | used for |
|---|---|
| SDL2, SDL2_image, OpenGL | window, input, rendering, PNG loading |
| OpenAL, vorbis, ogg, theora | sound, music, intro video |
| zlib | save compression |
| curl | HTTP(S) |
| **openssl** (`-lssl -lcrypto`) | TLS, HMAC and SHA-256 for the online layer |
| **liblzma** (`-llzma`) | compression for the dungeon transport |

The last two are additions of this fork - upstream KeeperRL links only `-lssl`.

## Building by hand

```bash
mkdir -p obj/extern obj-opt/extern      # the Makefile does not create these
make -j$(nproc) NO_STEAMWORKS=true RELEASE=true OPT=true GCC=clang++
```

`mkdir` matters: git cannot store empty directories, so a fresh clone can be missing
them, and the build then fails with `unable to open output file 'obj-opt/....o'`.

Useful flags:

| flag | effect |
|---|---|
| `RELEASE=true OPT=true` | optimised build (what you want) |
| `NO_STEAMWORKS=true` | build without the Steam SDK |
| `GCC=clang++` | use clang; drop it to build with g++ |
| `DEBUG=true` | debug build, no optimisation |
| `-j$(nproc)` | parallel compile |

Build output goes to `obj-opt/` (`obj/` for debug). `make clean` removes it.

## Artwork is not included

This repository contains **no KeeperRL artwork, music or sound** - those are commercial
assets and not redistributable. Without them the game runs in **ASCII**, which the engine
falls back to automatically when the `data` folder is absent.

To play with graphics, copy the `data` folder from a purchased copy of KeeperRL
(<https://keeperrl.com> or Steam) into the repository directory:

```
keeperrl/
├── keeper
├── appconfig.txt
├── data_free/        included
├── data_contrib/     included (fonts, SIL Open Font License)
├── mods/             included
└── data/             ← copy this in yourself: images/ music/ sound/ intro.ogv
```

## Online configuration (`appconfig.txt`)

| key | meaning |
|---|---|
| `server_list_url` | public list of servers, fetched at login; supplies the **address only** |
| `server_url` | fallback used when that list cannot be reached |
| `server_cert_pin` | pinned public key of the server certificate - a wrong server fails the check |
| `server_psk` | pre-shared key for the pre-TLS knock |

The address comes from the list; the pin and PSK always come from this file, so a
tampered list cannot redirect anyone to another server.

`server_url` defaults to `https://localhost:38552` - useful if you run your own server.
It must not be empty: an empty value would leave the game with no server at all.

## Running your own server

The same binary is the server:

```bash
./keeper --rar_server 38552
```

It keeps its state in `server/` beside the working directory: one folder per keeper under
`saves/<account>/<keeper>/`, plus the world and villain data. Clients need that machine's
`server_cert_pin` and `server_psk` in their own `appconfig.txt`.

## Troubleshooting

**`unable to open output file 'obj-opt/....o'`**
The build directories are missing: `mkdir -p obj/extern obj-opt/extern`.

**`fatal error: 'SDL.h' file not found`**
`libsdl2-dev` is not installed. The Makefile expects the headers in `/usr/include/SDL2`.

**Everything on screen is the same image**
A mod is supplying sprites while the base tileset is absent. Either add the `data`
folder, or run with `--free_mode` to force ASCII.

**Game starts but there is no login prompt**
`server_url` is empty in `appconfig.txt`. Set it, or leave the key out entirely so the
built-in `https://localhost:38552` default applies.
