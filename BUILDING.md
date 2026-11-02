# Building pim-32

pim-32 is a bare-metal kernel built on [Circle](https://github.com/rsta2/circle). It is cross-compiled on a PC with the Arm GNU Toolchain; there is no Linux image involved. These steps match what CI does (see `.github/workflows/ci.yml`) and were tested on Ubuntu 24.04 (x86_64). Other Linux distributions and WSL2 on Windows should work the same way.

## 1. Install host packages

```sh
sudo apt-get update
sudo apt-get install -y build-essential git cmake make patch pkg-config \
    python3 curl xz-utils texinfo
```

## 2. Install the Arm GNU Toolchain

pim-32 is built with **Arm GNU Toolchain 15.2.rel1**, the version Circle recommends and is tested with. Older toolchains may still compile it, but releases are built and tested with 15.2.rel1 only. You need the `aarch64-none-elf` toolchain for 64-bit boards and `arm-none-eabi` for 32-bit boards; installing both is simplest.

```sh
VER=15.2.rel1
for TRIPLET in aarch64-none-elf arm-none-eabi; do
    curl -fsSL "https://developer.arm.com/-/media/Files/downloads/gnu/$VER/binrel/arm-gnu-toolchain-$VER-x86_64-$TRIPLET.tar.xz" \
        | tar -xJ -C "$HOME"
done

export PATH="$HOME/arm-gnu-toolchain-$VER-x86_64-aarch64-none-elf/bin:$HOME/arm-gnu-toolchain-$VER-x86_64-arm-none-eabi/bin:$PATH"
```

Add the `export PATH=...` line to your shell profile to keep it. Check with `aarch64-none-elf-gcc --version`.

## 3. Get the source

```sh
git clone https://github.com/macroexp/mt32-pi.git pim-32
cd pim-32
make submodules
```

## 4. Build

```sh
make -j"$(nproc)" BOARD=pi3-64
```

The first build compiles Circle, newlib, Munt and FluidSynth and takes several minutes. Later builds only recompile what changed. The kernel image is written to the top of the source tree:

| Board | `BOARD=` | Kernel image | Shipped in releases |
| --- | --- | --- | --- |
| Pi Zero 2 W, Pi 3 (64-bit) | `pi3-64` (default) | `kernel8.img` | Yes |
| Pi 4, Pi 400, CM4 (64-bit) | `pi4-64` | `kernel8-rpi4.img` | Yes |
| Pi 2 | `pi2` | `kernel7.img` | Yes |
| Pi 3 (32-bit) | `pi3` | `kernel8-32.img` | No |
| Pi 4 (32-bit) | `pi4` | `kernel7l.img` | No |

Releases boot Pi 3 and Pi 4 with the 64-bit kernels (see `sdcard/config.txt`); the 32-bit targets are built in CI only to keep them compiling.

### Build options

Pass these on the `make` command line, e.g. `make BOARD=pi4-64 HDMI_CONSOLE=1`:

| Option | Default | Effect |
| --- | --- | --- |
| `HDMI_CONSOLE` | `0` | `1` sends the log to the HDMI output, useful for debugging |
| `GZIP_KERNEL` | `1` | Compress the kernel image (64-bit kernels only) |
| `GC_SECTIONS` | `1` | Remove unused code and data at link time |

### Switching boards

Dependencies are configured for one board at a time. Clean everything before building for a different board:

```sh
make mrproper
make -j"$(nproc)" BOARD=pi2
```

`make clean` removes only the kernel's own object files, for a quicker rebuild on the same board.

## 5. Try it on a Pi

The easiest way to get a bootable SD card is to download the latest release from [Releases](https://github.com/macroexp/mt32-pi/releases), extract it to a FAT32-formatted card, and copy your newly built kernel image over the one from the release.

## Notes for contributors

- Dependencies are git submodules under `external/`. Local fixes to them live in `patches/` and are applied automatically during the build; `make mrproper` reverses them. Run `make mrproper` *before* checking out a commit that changes a submodule or the patch list, so the old patches are reversed with the old `Makefile`; then `git submodule update`.
- The project builds with `-Werror`, so new compiler warnings in `src/` or `include/` fail the build.
- Python and shell scripts in `scripts/` are checked in CI with Black (`--preview`, pinned version), isort, flake8 and ShellCheck.
