# loader

fm1buddy's and lilfm1buddy's flash loader: it runs in the FM-1's RAM where the chip's boot ROM hands over its USB
storage commands, the same place, and the same commands, as JieLi's `wl82loader.bin`. The board sends it there, runs it,
and writes or reads the flash through it (fm1buddy's `src/app/uboot.cpp`, lilfm1buddy's `esp32/rom.cpp`: it builds it
from here, its `third_party/fm1buddy`).

It's [m-bootloader](https://github.com/jvitkauskas/m-bootloader)'s flash loader (MIT), from commit
`d131391a398dc9a757b54ea91ab3cc71ef939d14` (release `2.0-c43a32f`): `src/` and `include/` are its `loader`,
`loader_target`, `display`, `platform`, `core` and `runtime`, and `tools/target.ld.S` its link script. `LICENSE` is its
(MIT). Its specification, `docs/SPEC.md` there, is the protocol.

What lilfm1buddy and fm1buddy add:

- `FC47`: the flash as 16 × 16 sectors of 4 KiB, a byte a sector, drawn as it's read or written (`display_map` in
  `src/display.c`), so the FM-1 shows what's going on instead of a still screen. Colours, one a state: dark, not
  touched; amber, to write; green, written; dim green, already the same; cyan, read; red, failed. The board draws the
  two rows above it (`FC44`), with the percentage.
- `FC48`: the chip's own reset (power-control bit 4), so it starts its firmware again when the job's done, as switching
  it off and on does. The loader has no JieLi `RUN_APP`: without this, it stayed in the boot ROM at the end. Its
  answer goes out first; the host may not see it.
- `FC49` (fm1buddy's): the screen's colours, its theme's: from an index in the command (0 the background, 1 to 7 a map
  state's), up to 6 big-endian RGB565 colours. Until it's sent, the colours above. fm1buddy's map states are its own
  (`src/app/jobs.cpp`, `FmState`), drawn in the colours it sends.
- A `%` in the font.

`test.sh` builds `loader.c` and `core.c` for this computer and checks those commands, the screen's rows, and what the
board asks of it (`test.c`): `CC=clang loader/test.sh`.

Build it (the toolchain is [pi32v2-llvm](https://github.com/jvitkauskas/pi32v2-llvm), `pi32v2-23.1.5-2`, not JieLi's):

```sh
PI32V2_TOOLCHAIN=~/pi32v2-llvm loader/build.sh   # build/mflr-loader.bin, and src/app/mflr_loader.h (in the board's program)
```

`src/app/mflr_loader.h` is the same bytes, in the board's program: commit it with the sources. Its build isn't the
same on every host (a few bytes of padding: 6312 on macOS arm64, 6316 on Linux x64, both fine), so CI builds the
loader but doesn't compare the header byte for byte: run `loader/build.sh` and commit the header after changing
anything here.

The boards have the loader in their programs, so nothing has to be downloaded (no Wi-Fi). lilfm1buddy also takes a
`mflr-loader.bin` on its drive instead, if there is one: a newer build, to try without flashing the board.
