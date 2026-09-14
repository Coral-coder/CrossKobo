#!/bin/sh
#
# Builds the installable packages for a Kobo.
#
# Outputs, in release/:
#   Catalogues-<ver>-install.zip       catalogues in the Kobo's own browser
#   Catalogues-<ver>-uninstall.zip
#   KoboRoot.tgz                       the CrossKobo payload, for manual copying
#   CrossKobo-<ver>-install.zip        unzip to the root of the Kobo drive
#   CrossKobo-<ver>-uninstall.zip      unzip to the root of the Kobo drive
#   CrossKobo-<ver>-fw5-install.zip    same, for firmware 5.x devices
#   CrossKobo-<ver>-fw5-uninstall.zip
#
# Usage: scripts/package.sh [path-to-arm-binary]
#        (default: build-arm/crosskobo; the catalogues binary is expected
#        next to it)

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
# Only the packages are rebuilt. A fetcher built earlier (scripts/build-fetcher.sh)
# and a NickelMenu release fetched earlier (scripts/fetch-nickelmenu.sh) stay
# where they are, so packaging can be re-run without the network.
mkdir -p "${OUT}"
rm -f "${OUT}"/*.zip "${OUT}"/KoboRoot.tgz

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
# Certificates for the fetcher, and the fetcher itself when CI has built
# one. Without a bundled fetcher CrossKobo falls back to whatever the
# firmware has; with it, https works the same on every device.
install -m 644 third_party/cacert.pem "${PAYLOAD}/usr/local/crosskobo/cacert.pem"
if [ -x "${OUT}/bin/curl" ]; then
    mkdir -p "${PAYLOAD}/usr/local/crosskobo/bin"
    install -m 755 "${OUT}/bin/curl" "${PAYLOAD}/usr/local/crosskobo/bin/curl"
    echo "bundling the fetcher: $(ls -la "${OUT}/bin/curl" | awk '{print $5}') bytes"
else
    echo "no fetcher built (scripts/build-fetcher.sh); packaging without one"
fi
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
# Catalogues: its own program, in its own package, with nothing of the
# CrossKobo interface in it.
#
# A web server on the loopback address, serving book catalogues to the
# Kobo's own browser - so the touch, the keyboard and the scrolling are the
# stock software's, and the only thing added is the books. Nothing runs at
# boot: the server starts from the menu entry and stops when idle. NickelMenu
# rides along in the payload, so the menu entries are there without anything
# else to install.
# ---------------------------------------------------------------------------
CAT_BIN="$(dirname "${BIN}")/catalogues"
if [ ! -f "${CAT_BIN}" ]; then
    echo "package: no catalogues binary at ${CAT_BIN}" >&2
    exit 1
fi
CAT_PAY="${STAGE}/catalogues-payload"
mkdir -p "${CAT_PAY}/usr/local/catalogues"
install -m 755 "${CAT_BIN}" "${CAT_PAY}/usr/local/catalogues/catalogues"
if command -v arm-linux-gnueabihf-strip >/dev/null 2>&1; then
    arm-linux-gnueabihf-strip "${CAT_PAY}/usr/local/catalogues/catalogues"
fi
install -m 755 scripts/install/catalogues-start.sh "${CAT_PAY}/usr/local/catalogues/start.sh"
install -m 755 scripts/install/catalogues-uninstall.sh \
    "${CAT_PAY}/usr/local/catalogues/uninstall.sh"
install -m 644 third_party/cacert.pem "${CAT_PAY}/usr/local/catalogues/cacert.pem"
if [ -x "${OUT}/bin/curl" ]; then
    mkdir -p "${CAT_PAY}/usr/local/catalogues/bin"
    install -m 755 "${OUT}/bin/curl" "${CAT_PAY}/usr/local/catalogues/bin/curl"
fi
printf '%s\n' "${VERSION}" > "${CAT_PAY}/usr/local/catalogues/VERSION"
chmod 644 "${CAT_PAY}/usr/local/catalogues/VERSION"
# NickelMenu, as released, on top. Its own tgz puts libnm.so where nickel
# loads plugins from and its documentation in .adds/nm on the drive.
if [ -f "${OUT}/nickelmenu/KoboRoot.tgz" ]; then
    tar -xzf "${OUT}/nickelmenu/KoboRoot.tgz" -C "${CAT_PAY}"
    echo "bundling NickelMenu $(cat "${OUT}/nickelmenu/VERSION" 2>/dev/null || echo '?')"
else
    echo "no NickelMenu fetched (scripts/fetch-nickelmenu.sh); packaging without it"
fi
tar --owner=root --group=root --numeric-owner -czf "${STAGE}/catalogues-KoboRoot.tgz" \
    -C "${CAT_PAY}" .

CAT="${STAGE}/catalogues"
mkdir -p "${CAT}/.kobo" "${CAT}/.adds/nm" "${CAT}/.adds/catalogues"
cp "${STAGE}/catalogues-KoboRoot.tgz" "${CAT}/.kobo/KoboRoot.tgz"
install -m 644 scripts/install/nm-catalogues "${CAT}/.adds/nm/catalogues"
# Example files ONLY - never the live catalogues.txt / shelfmark.txt. The
# program creates those itself, once, if they are missing, and then leaves
# them alone; the zip must not carry them or every re-extract would wipe
# out the servers the reader has added. These .example files are safe to
# overwrite because nobody edits them - they are just something to copy a
# line out of.
cat > "${CAT}/.adds/catalogues/catalogues.txt.example" <<'TXT'
# Catalogues - OPDS feeds (browsed from the Catalogues menu entry).
#
# This is an EXAMPLE. Your real list is catalogues.txt (no .example), which
# the app creates once and never overwrites. Copy lines you want into it.
#
# One server per line, a hostname rather than an IP number:
#
#   Name | https://host/opds
#   Name | https://host/opds | user | password
#
# Anything that is not an OPDS feed - a Library-Genesis-style search server
# - belongs in shelfmark.txt instead, and is used from the Shelfmark entry.
#
# Public OPDS catalogues you might want (copy into catalogues.txt to use):
# Project Gutenberg | https://m.gutenberg.org/ebooks.opds/
# Internet Archive  | https://bookserver.archive.org/catalog/
TXT
chmod 644 "${CAT}/.adds/catalogues/catalogues.txt.example"
cat > "${CAT}/.adds/catalogues/shelfmark.txt.example" <<'TXT'
# Shelfmark - search servers (used from the Shelfmark menu entry).
#
# This is an EXAMPLE. Your real list is shelfmark.txt (no .example), which
# the app creates once and never overwrites. Copy a line into it.
#
# Shelfmark is a search engine set up exactly like Library Genesis: give it
# your server's address - the base URL, or the full search route - and it
# searches there and downloads. One per line, hostnames not IP numbers:
#
#   My server | https://fic.example.net
#   My server | https://fic.example.net/search?q={searchTerms}
#   My server | https://fic.example.net/search.php?req={searchTerms} | user | password
#
# No servers are shipped - add your own to shelfmark.txt.
TXT
chmod 644 "${CAT}/.adds/catalogues/shelfmark.txt.example"
cat > "${CAT}/READ-ME-FIRST.txt" <<TXT
Catalogues ${VERSION}
=================

Book catalogues - Shelfmark, Calibre-Web, Kavita, Komga, Project Gutenberg,
and a search-format server of your own - in your Kobo's own browser. The
Kobo software stays exactly as it is; this adds entries to its menu.

  1. Eject the Kobo safely and unplug it.
  2. It installs and restarts by itself. Give it a minute.
  3. On the home screen, open the NickelMenu tab at the bottom right and
     tap "Catalogues" (or "Shelfmark", once you have added one).

Everything is done in the Kobo's browser: tap a catalogue, tap a folder,
tap a book, tap Download. Searching uses the Kobo's own keyboard.

Your servers go in two files on this drive, which the app creates once and
then never overwrites - so updating never wipes your list:
  .adds/catalogues/catalogues.txt   OPDS feeds (the Catalogues entry)
  .adds/catalogues/shelfmark.txt    search servers (the Shelfmark entry)
This zip ships only *.example copies of them to crib lines from; your real
files are left alone. A friend can send you a line to paste in.

Downloads land in the Downloads folder on this drive. Tap Sync on the home
screen, or "Catalogues - add downloads to library" in the menu, and they
appear in your library.

NickelMenu (https://github.com/pgaskin/NickelMenu, MIT licence) is
installed with this; it is what puts the entries in the menu. If you
already had it, nothing changes.

To remove: unzip Catalogues-${VERSION}-uninstall.zip onto this drive and
eject. After the restart, tap "Remove Catalogues" in the menu. Nothing
runs at boot, so nothing can leave the device stuck.
TXT
(cd "${CAT}" && zip -q -r "${OUT}/Catalogues-${VERSION}-install.zip" \
    .kobo .adds READ-ME-FIRST.txt)

# The uninstaller: a fresh copy of the removal script, and a menu that
# offers it. The KoboRoot.tgz makes the firmware restart the device, which
# is when NickelMenu reads the new menu. No boot hook, nothing that could
# stand between the device and its own software.
CAT_UN_PAY="${STAGE}/catalogues-uninstall-payload"
mkdir -p "${CAT_UN_PAY}/usr/local/catalogues"
install -m 755 scripts/install/catalogues-uninstall.sh \
    "${CAT_UN_PAY}/usr/local/catalogues/uninstall.sh"
tar --owner=root --group=root --numeric-owner -czf "${STAGE}/catalogues-uninstall-KoboRoot.tgz" \
    -C "${CAT_UN_PAY}" .
CAT_UN="${STAGE}/catalogues-uninstall"
mkdir -p "${CAT_UN}/.kobo" "${CAT_UN}/.adds/nm"
cp "${STAGE}/catalogues-uninstall-KoboRoot.tgz" "${CAT_UN}/.kobo/KoboRoot.tgz"
install -m 644 scripts/install/nm-catalogues-remove "${CAT_UN}/.adds/nm/catalogues"
cat > "${CAT_UN}/READ-ME-FIRST.txt" <<TXT
Catalogues ${VERSION} uninstaller

Eject the Kobo and unplug it; it restarts. Then open the NickelMenu tab
and tap "Remove Catalogues". That takes away the program and its menu
entries. Your books, the Downloads folder and .adds/catalogues (your
catalogue file) are left alone.

NickelMenu stays. To remove it as well, put an empty file named
"uninstall" in the .adds/nm folder on this drive and restart.
TXT
(cd "${CAT_UN}" && zip -q -r "${OUT}/Catalogues-${VERSION}-uninstall.zip" \
    .kobo .adds READ-ME-FIRST.txt)

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
