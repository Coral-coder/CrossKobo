#!/bin/sh
#
# Builds the installable packages for a Kobo.
#
# Outputs, in release/:
#   KoboRoot.tgz                       the payload, for manual copying
#   CrossKobo-<ver>-install.zip        unzip to the root of the Kobo drive
#   CrossKobo-<ver>-uninstall.zip      unzip to the root of the Kobo drive
#   CrossKobo-<ver>-fw5-install.zip    same, for firmware 5.x devices
#   CrossKobo-<ver>-fw5-uninstall.zip
#
# Usage: scripts/package.sh [path-to-arm-binary]
#        (default: build-arm/crosskobo)

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

BIN="${1:-build-arm/crosskobo}"
VERSION="$(sed -n 's/^const char\* kVersion = "\([^"]*\)".*/\1/p' src/core/version.cpp | head -1)"
[ -n "${VERSION}" ] || VERSION="0.0.0"

if [ ! -f "${BIN}" ]; then
    echo "package: no binary at ${BIN}" >&2
    echo "build one first:" >&2
    echo "  cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/kobo-armhf.cmake" >&2
    echo "  cmake --build build-arm -j\$(nproc)" >&2
    exit 1
fi

case "$(file -b "${BIN}")" in
    *ARM*statically*) ;;
    *) echo "package: warning: ${BIN} is not a static ARM binary" >&2 ;;
esac

STAGE="$(mktemp -d)"
OUT="${ROOT}/release"
trap 'rm -rf "${STAGE}"' EXIT
rm -rf "${OUT}"
mkdir -p "${OUT}"

# ---------------------------------------------------------------------------
# The payload: everything that lands on the device's root filesystem.
# ---------------------------------------------------------------------------
PAYLOAD="${STAGE}/payload"
mkdir -p "${PAYLOAD}/usr/local/crosskobo/fonts"
mkdir -p "${PAYLOAD}/etc/init.d" "${PAYLOAD}/etc/rcS.d"

install -m 755 "${BIN}" "${PAYLOAD}/usr/local/crosskobo/crosskobo"
# Strip the binary: it goes from ~17 MB to ~2 MB, which matters on a device
# whose rootfs has little free space.
if command -v arm-linux-gnueabihf-strip >/dev/null 2>&1; then
    arm-linux-gnueabihf-strip "${PAYLOAD}/usr/local/crosskobo/crosskobo"
fi
install -m 755 scripts/install/crosskobo.sh "${PAYLOAD}/usr/local/crosskobo/crosskobo.sh"
install -m 755 scripts/install/start-nickel.sh "${PAYLOAD}/usr/local/crosskobo/start-nickel.sh"
install -m 755 scripts/install/menu-launch.sh "${PAYLOAD}/usr/local/crosskobo/menu-launch.sh"
install -m 644 scripts/install/nm-crosskobo "${PAYLOAD}/usr/local/crosskobo/nm-crosskobo"
# SemiBold would only duplicate the bold slot, so it is left out of the
# package to keep the payload small.
for f in assets/fonts/*.ttf; do
    case "${f}" in
        *SemiBold*) continue ;;
    esac
    install -m 644 "${f}" "${PAYLOAD}/usr/local/crosskobo/fonts/"
done
install -m 644 assets/fonts/LICENSE-*.txt "${PAYLOAD}/usr/local/crosskobo/fonts/"
printf '%s\n' "${VERSION}" > "${PAYLOAD}/usr/local/crosskobo/VERSION"
chmod 644 "${PAYLOAD}/usr/local/crosskobo/VERSION"

# Boot hooks for both firmware layouts. The firmware 4 hook replaces the
# animation script; the firmware 5 hook is an init script plus a symlink,
# and is inert on firmware 4.
install -m 755 scripts/install/on-animator.sh "${PAYLOAD}/etc/init.d/on-animator.sh"
install -m 755 scripts/install/crosskobo.init "${PAYLOAD}/etc/init.d/crosskobo"
ln -sf ../init.d/crosskobo "${PAYLOAD}/etc/rcS.d/S99crosskobo"

tar --owner=root --group=root --numeric-owner -czf "${OUT}/KoboRoot.tgz" -C "${PAYLOAD}" .

# ---------------------------------------------------------------------------
# Firmware 4 install package: a zip that extracts straight onto the drive.
# ---------------------------------------------------------------------------
PKG="${STAGE}/install"
mkdir -p "${PKG}/.kobo"
cp "${OUT}/KoboRoot.tgz" "${PKG}/.kobo/KoboRoot.tgz"
cat > "${PKG}/READ-ME-FIRST.txt" <<TXT
CrossKobo ${VERSION}
====================

You have already done the hard part: if this file is sitting in the root of
your Kobo's drive, the installer is in place too.

  1. Eject the Kobo safely and unplug it.
  2. The device installs CrossKobo and restarts by itself. Give it a minute.
  3. CrossKobo starts instead of the Kobo home screen.

To get back to the stock Kobo software, either:

  * open Settings and choose "Return to the Kobo UI" (just for this session), or
  * plug the Kobo into a computer, create an empty folder named
    ".crosskobo" if it is not there, and put an empty file called DISABLE
    inside it. CrossKobo then stays out of the way until you delete it, or
  * unzip the CrossKobo uninstaller onto the drive and eject: that removes
    CrossKobo completely and leaves your books and notebooks alone.

Your notebooks are saved in the "Notebooks" folder on this drive, as .ckn
files, and can be exported to PDF or PNG from inside CrossKobo.

Full documentation: docs/INSTALL.md in the CrossKobo repository.
TXT
(cd "${PKG}" && zip -q -r "${OUT}/CrossKobo-${VERSION}-install.zip" .kobo READ-ME-FIRST.txt)

# ---------------------------------------------------------------------------
# Firmware 4 uninstall package.
# ---------------------------------------------------------------------------
UNPAY="${STAGE}/uninstall-payload"
mkdir -p "${UNPAY}/etc/init.d"
install -m 755 scripts/install/on-animator-uninstall.sh "${UNPAY}/etc/init.d/on-animator.sh"
tar --owner=root --group=root --numeric-owner -czf "${STAGE}/uninstall-KoboRoot.tgz" \
    -C "${UNPAY}" .
UNPKG="${STAGE}/uninstall"
mkdir -p "${UNPKG}/.kobo"
cp "${STAGE}/uninstall-KoboRoot.tgz" "${UNPKG}/.kobo/KoboRoot.tgz"
cat > "${UNPKG}/READ-ME-FIRST.txt" <<TXT
CrossKobo ${VERSION} uninstaller

Eject the Kobo and unplug it. On the next boot CrossKobo removes itself
from the device and the stock Kobo software comes back.

Your books, notebooks (the Notebooks folder) and CrossKobo settings (the
.crosskobo folder) are left untouched. Delete them by hand if you want them
gone.
TXT
(cd "${UNPKG}" && zip -q -r "${OUT}/CrossKobo-${VERSION}-uninstall.zip" .kobo READ-ME-FIRST.txt)

# ---------------------------------------------------------------------------
# Firmware 5 packages, which use .kobo/update.tar instead of KoboRoot.tgz.
# ---------------------------------------------------------------------------
FW5="${STAGE}/fw5"
mkdir -p "${FW5}/.kobo"
cp "${OUT}/KoboRoot.tgz" "${STAGE}/crosskobo.tgz"
tar --owner=root --group=root --numeric-owner -cf "${FW5}/.kobo/update.tar" \
    -C scripts/install driver.sh crosskobo-install.sh
tar --owner=root --group=root --numeric-owner -rf "${FW5}/.kobo/update.tar" \
    -C "${STAGE}" crosskobo.tgz
cat > "${FW5}/READ-ME-FIRST.txt" <<TXT
CrossKobo ${VERSION} for Kobo firmware 5.x

Eject the Kobo and unplug it; the device installs CrossKobo on the next
boot. See READ-ME-FIRST in the standard package for how to get back to the
stock software.
TXT
(cd "${FW5}" && zip -q -r "${OUT}/CrossKobo-${VERSION}-fw5-install.zip" .kobo READ-ME-FIRST.txt)

FW5UN="${STAGE}/fw5-uninstall"
mkdir -p "${FW5UN}/.kobo"
cat > "${STAGE}/fw5-driver.sh" <<'TXT'
#!/bin/sh
set -e
ARCHIVE="$1"
STAGE="$2"
mkdir -p /tmp/updater
case "${STAGE}" in
    stage1)
        tar -C /tmp/updater -xf "${ARCHIVE}" "crosskobo-uninstall.sh"
        /bin/sh /tmp/updater/crosskobo-uninstall.sh
        ;;
esac
sync
exit 1
TXT
# The updater looks for driver.sh, so rename it inside the archive.
tar --owner=root --group=root --numeric-owner --transform 's|fw5-driver.sh|driver.sh|' \
    -cf "${FW5UN}/.kobo/update.tar" -C "${STAGE}" fw5-driver.sh
tar --owner=root --group=root --numeric-owner -rf "${FW5UN}/.kobo/update.tar" \
    -C scripts/install crosskobo-uninstall.sh
(cd "${FW5UN}" && zip -q -r "${OUT}/CrossKobo-${VERSION}-fw5-uninstall.zip" .kobo)

# ---------------------------------------------------------------------------
echo "CrossKobo ${VERSION} packaged in release/:"
ls -la "${OUT}"
echo
echo "KoboRoot.tgz contents:"
tar -tzvf "${OUT}/KoboRoot.tgz"
