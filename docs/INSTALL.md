# Installing CrossKobo

CrossKobo replaces the interface on your Kobo, not the firmware. The Kobo's
own Linux system, kernel and drivers stay exactly as they are; CrossKobo is
a program that starts at boot instead of the stock reading software, and it
can be switched off again from a computer in under a minute.

**Read the [Before you start](#before-you-start) and
[If something goes wrong](#if-something-goes-wrong) sections first.** This
release has not been run on real hardware yet.

---

## Before you start

1. **Check your firmware version.** On the Kobo: *More → Settings → Device
   information*. Anything in the 4.x series uses the standard package.
   Firmware 5.x uses the `-fw5-` package instead. If in doubt, install the
   standard package: on a 5.x device it simply does nothing, and you can
   then try the other one.
2. **Back up your books.** Plug the Kobo into a computer and copy the whole
   drive somewhere. This takes a few minutes and means the worst case is an
   inconvenience rather than a loss.
3. **Know where the escape hatch is.** After installing, the folder
   `.crosskobo` appears on the Kobo's drive. Putting an empty file called
   `DISABLE` in it stops CrossKobo from starting. You can create that file
   at any time from a computer, including *before* you install.
4. **Charge the device** above 50%. An interrupted first boot is the one
   thing worth avoiding.

## Install

1. Download `CrossKobo-<version>-install.zip` from the
   [releases page](https://github.com/coral-coder/crosskobo/releases)
   (or `CrossKobo-<version>-fw5-install.zip` for firmware 5.x).
2. Plug the Kobo into your computer. It appears as a USB drive, usually
   called `KOBOeReader`.
3. **Unpack the zip into the root of that drive** — not into a subfolder.
   Use your file manager's "Extract here" on the drive itself. When it is
   done, the drive contains a hidden folder `.kobo` with `KoboRoot.tgz`
   inside it, and a `READ-ME-FIRST.txt` you can delete afterwards.
   - On Windows, hidden folders may not be visible. Extracting still works;
     you just will not see `.kobo` unless hidden items are shown.
   - Do not drag `KoboRoot.tgz` out of the zip by hand into the wrong place.
     It must end up at `<drive>/.kobo/KoboRoot.tgz`.
4. **Eject the drive safely** and unplug the cable.
5. The Kobo shows its usual "installing" screen, restarts, and comes up in
   CrossKobo. The first boot takes a little longer than usual — up to a
   minute or two — because the stock software starts first and CrossKobo
   takes over once the system has settled.

That is the whole installation. Your books, and Kobo's own database, are
untouched.

## First run

- The **home screen** shows what you were last reading and a grid of recent
  books. Tap *Library* to browse everything on the device.
- Books go where they always did: anywhere on the Kobo drive, in folders if
  you like. CrossKobo reads `.epub`, `.txt`, `.md` and `.cbz`.
- **Notebooks** live in a `Notebooks` folder on the drive, as `.ckn` files
  (plain JSON). They are yours to copy, back up or delete from a computer.
- If taps land in the wrong place, go to *Settings → Controls → Touch and
  stylus test*. Tap the circles: if the marks appear mirrored, toggle
  *mirror X* or *mirror Y* until they land under your finger. Do the same
  with the stylus. This matters because the panel and digitiser orientation
  differ between Kobo models and this build has not been calibrated against
  each of them.

## Moving files on and off the device

The stock Kobo software owns the USB mass-storage machinery, so CrossKobo
hands over to it rather than reimplementing it:

1. Plug the Kobo into a computer while CrossKobo is running.
2. CrossKobo asks whether to switch to the Kobo UI. Choose *Switch*.
3. The stock software starts and the drive appears on your computer as
   usual. Copy books, notebooks or anything else.
4. Eject, then power the Kobo off and on. CrossKobo comes back.

You can change this behaviour in *Settings → Controls → When USB is
connected*: *Ask* (default), *Switch to Kobo UI*, or *Ignore* (useful if you
only ever plug in to charge).

## Getting back to the stock software

Three options, in increasing order of permanence:

**Just for now** — *Settings → Return to the Kobo UI*. The stock software
starts immediately. CrossKobo comes back on the next power-on.

**Until further notice** — put an empty file named `DISABLE` in the
`.crosskobo` folder on the Kobo drive (from a computer), then reboot.
CrossKobo stays out of the way, changing nothing else, until you delete the
file.

**Remove it** — unpack `CrossKobo-<version>-uninstall.zip` to the root of
the drive and eject. On the next boot CrossKobo deletes itself from the
device and restores the stock boot script. Your books, notebooks and
CrossKobo settings are left alone; delete the `Notebooks` and `.crosskobo`
folders by hand if you want them gone too.

## After a Kobo firmware update

A Kobo firmware update rewrites the device's root filesystem, which removes
CrossKobo's boot hook. The symptom is simple: the Kobo comes back up in its
own software. Reinstall CrossKobo the same way as before, once you are
happy the new firmware works. Your books, notebooks and settings are on the
user partition and survive firmware updates untouched.

## If something goes wrong

CrossKobo is built so that the device always ends up in a usable state.

**CrossKobo does not start; the Kobo behaves normally.** The boot hook
stood down. Look at `.crosskobo/boot.log` on the drive (and
`/usr/local/crosskobo/boot.log` if you have shell access) — it records why.
The usual reasons are a `DISABLE` file, or the crash-loop guard having
tripped.

**CrossKobo crashed and the Kobo UI came back.** That is the fallback
working. `.crosskobo/crosskobo.log` has the detail. Three failed starts in
a row and CrossKobo writes its own `DISABLE` file and stops trying, so a
bad build cannot lock you out.

**The screen is stuck or nothing responds.** Hold the power button for
about 30 seconds to force a power-off, then press it again. If the device
comes back up in CrossKobo and immediately misbehaves, plug it into a
computer: the drive still mounts in that state because the stock software
is what handles USB, and once a `DISABLE` file exists, the next boot is
stock.

**The device will not mount over USB at all.** Power it off completely
(hold power ~30 seconds). Hold the power button and plug the cable in; the
Kobo boots and mounts before the interface starts, which is enough to place
the `DISABLE` file.

**Nothing works.** A Kobo factory reset restores the firmware from the
device's recovery partition and removes anything installed to the root
filesystem, CrossKobo included: hold the power button until the device
powers off, then follow Kobo's own factory-reset procedure for your model
(*Settings → Device information → Factory reset* from the stock software,
or the hardware procedure Kobo documents). You will lose the books on the
device, which is why the backup in step 2 is worth the five minutes.

## What the installer actually does

For the curious and the cautious, the whole payload is these files:

```
/usr/local/crosskobo/crosskobo        the program (static ARM binary)
/usr/local/crosskobo/crosskobo.sh     the launcher, with the safety guards
/usr/local/crosskobo/start-nickel.sh  brings the stock UI back
/usr/local/crosskobo/fonts/           bundled reading fonts
/etc/init.d/on-animator.sh            boot hook (firmware 4.x)
/etc/init.d/crosskobo                 boot hook (firmware 5.x)
/etc/rcS.d/S99crosskobo               symlink that runs the above
```

`/etc/init.d/on-animator.sh` is the script the firmware runs to draw the
boot animation. CrossKobo replaces it with a version that starts the
launcher and then draws the same animation — the mechanism fmon, KFMon and
Kobo Start Menu have used for years. The uninstaller puts the stock script
back.

Nothing else on the device is modified. No firmware files are patched, no
partitions are touched, and the Kobo database is left exactly as it was.
