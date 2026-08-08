KeeperRL Online
===============

Hello, and welcome to KeeperRL Online.

**This is not an official version of KeeperRL.** It is a spin-off — a mod, if you prefer — that lets other
keepers play in the same world and destroy each other. It is recommended for more experienced players.

Your dungeon lives on a shared, server-authoritative world map alongside everyone else's. Other keepers can
find you and invade you, and you can do the same to them — including while they are offline. Their base is
defended by the minions and traps they left behind.

Download
--------

**[Latest release](https://github.com/samuellsk/keeperrl-online/releases/latest)** — Windows, unzip anywhere
and run `keeper.exe`. Everything needed to play is in the archive.

To build from source (Linux or Windows) see **[BUILDING.md](BUILDING.md)**. On Ubuntu or Debian,
`./build-ubuntu.sh` installs the dependencies and compiles in one step.

Graphics
--------

**This version does not include graphics.** Artwork, music and sound are commercial assets and cannot be
redistributed here, so out of the box the game runs in **ASCII mode**.

It is strongly recommended that you buy the official version — [keeperrl.com](https://keeperrl.com) or
[Steam](https://store.steampowered.com/app/329970) — and copy its `data` folder across. Besides supporting the
people who made the game, you get the real tiles, music and sound.

### Where the `data` folder goes

Copy it **next to the other `data_*` folders**, i.e. into the same folder as `keeper.exe`:

```
keeperrl-online-0.0.1-win64/
├── keeper.exe
├── appconfig.txt
├── data_contrib/        (ships with this release)
├── data_free/           (ships with this release)
├── mods/                (ships with this release)
└── data/                <-- copy this one in yourself
    ├── images/
    ├── music/
    ├── sound/
    └── intro.ogv
```

Copy the folder itself, not its contents — you want `data/images/`, not `images/` at the top level. All four
of those entries must be inside it.

Where to find it in the official game:

| bought from | copy from |
|---|---|
| Steam | `C:\Program Files (x86)\Steam\steamapps\common\KeeperRL\data` |
| keeperrl.com | the `data` folder in the game's install directory |

The game switches to graphics automatically the next time it starts — there is no setting to change. If it is
still in ASCII, `data` is in the wrong place or is missing one of the four entries above.

Note that if you build from source, the game reads `data`, `data_free` and `mods` from the **current working
directory**, not from wherever the binary is. Run it from the repository folder.

Bugs
----

If you find any bugs, please report them on Discord. Given that this is an alpha and no closed test has
happened yet, expect to find some.

Thank you.

— Rar (Samuell)

Credits and licence
-------------------

KeeperRL is by Michał Brzozowski — [keeperrl.com](https://keeperrl.com). This fork only adds the online layer
on top of his game; all credit for KeeperRL itself belongs to him and its contributors.

Source is under the GPL, see [LICENSE](LICENSE). Media terms are in [COPYING-MEDIA.txt](COPYING-MEDIA.txt) —
which is why no artwork ships here.
