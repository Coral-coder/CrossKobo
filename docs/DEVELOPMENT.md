# Development

CrossKobo is C++17 with no dependencies beyond libc: the only third-party
code is vendored in `third_party/` (stb for TrueType and image decoding,
miniz for ZIP). That is deliberate — the device build links statically, so
it does not care what vintage of glibc or Qt the firmware ships.

## Layout

```
src/core/        logging, strings and UTF-8, files, JSON, paths, clock
src/gfx/         canvas, colour, TrueType text, geometry
src/platform/    device detection, framebuffer and EPD, input, power, the stock UI
src/ui/          theme, immediate-mode widgets, list view, keyboard, icons
src/epub/        ZIP, XML, CSS subset, OPF/NCX/nav, layout and pagination
src/reader/      reading view, menus, per-book state, statistics
src/notes/       notebook model and storage, editor, browser, PDF/PNG export
src/library/     library scanning, cover cache, browser
src/app/         application shell, settings, home, statistics, calibration
src/sim/         host simulator
tests/           host tests
scripts/install/ boot hooks and the launcher that ship on the device
```

The rendering model is immediate mode: a view paints its whole area when
asked and rebuilds its hit-test list as it goes. There is no retained
widget tree, because on a display that only changes when told to, there is
nothing to keep in sync.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Device build, then packages:

```sh
sudo apt install g++-arm-linux-gnueabihf
cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/kobo-armhf.cmake
cmake --build build-arm -j"$(nproc)"
scripts/package.sh            # release/*.zip and release/KoboRoot.tgz
tests/test_package.sh         # checks the package shape
```

The toolchain file targets armv7-a with NEON, hard float, and links
statically. That covers every Kobo from the i.MX6 devices to the MT8113
ones, and avoids depending on the firmware's libraries.

## Testing without a Kobo

There are three layers, and between them they cover everything except the
ioctls themselves.

**Unit and integration tests** (`ctest`):
- `test_gfx` renders text, shapes and a tapered stroke, and checks the
  output is not blank.
- `test_epub` builds an EPUB in memory, opens it through the real
  `Book`/`Layout` stack, checks metadata, table of contents, `display:none`
  handling, CSS colour, pagination ordering, resume-position round-trips
  and that no line overflows its box, then renders pages to PNG.

**The simulator** runs the real application against an in-memory
framebuffer:

```sh
./build/crosskobo-sim --root out/sim-root --out out/sim
```

It writes a PNG of every screen — home in both themes, library, reader,
reader menu, notes editor, notebook list, settings, statistics, about,
calibration, night mode — plus a sample notebook exported to PDF. Reviewing
those images is how the interface is checked.

**The ARM binary under emulation**:

```sh
sudo apt install qemu-user-static
qemu-arm-static ./build-arm/crosskobo-sim --root out/arm-root --out out/arm
```

This runs the exact binary that ships, on the target architecture, and
produces the same screenshots. It catches endianness, alignment and
libc-version problems; it cannot catch anything about the panel or the
input devices.

**What remains untested** is `src/platform/screen.cpp` (the display
controller ioctls), `src/platform/input.cpp` (real digitiser data),
`src/platform/power.cpp` (sysfs and suspend) and the boot scripts. Those
need hardware. They are written defensively — every ioctl and sysfs write
is checked and logged, and failure degrades rather than crashes — and the
launcher has a crash-loop guard behind them.

## Known limits

- A chapter is laid out in one pass, so a book that puts five megabytes of
  text in a single XHTML file will pause for a few seconds when that
  chapter opens. Normal books split per chapter and paginate instantly.
- Notebook pages are a fixed pixel grid, adopted from the screen the
  notebook was created on, and scaled to fit elsewhere. Ink is never
  re-flowed.
- Tables are laid out as stacked cells rather than real columns.
- No network features, so no OPDS, sync or dictionaries; and no PDF
  reading (exporting notes to PDF is a different problem, and is
  supported).

## Adding a device

`src/platform/device.cpp` holds the model table, keyed on the codename
reported by `/bin/kobo_config.sh`. A new device needs its codename, dpi,
and whether it has page buttons, a stylus and a G-sensor. The display
controller family and the colour filter array are probed, not tabled.

If the display or input behaves oddly on a new model, the first stop is
*Settings → Controls → Touch and stylus test*, then
`.crosskobo/crosskobo.log`, which records the detected panel geometry,
pixel layout, controller family and every input device with its axis
ranges.

## Style

Follow what is there: four-space-free two-space indent, `snake_case` for
functions and variables, `CamelCase` for types, and comments that explain
why something is the way it is rather than restating the code. Anything
device-specific gets a comment saying where the knowledge came from.
