#!/bin/sh
#
# Checks the shape of the installable packages: the payload must land in the
# right places with the right permissions, and the uninstaller must remove
# exactly what the installer added. Run after scripts/package.sh.

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

fail=0
check() {
    if [ "$1" = "0" ]; then
        printf '  ok   %s\n' "$2"
    else
        printf '  FAIL %s\n' "$2"
        fail=$((fail + 1))
    fi
}

[ -f release/KoboRoot.tgz ] || { echo "run scripts/package.sh first" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# ---------------------------------------------------------------------------
echo "install payload:"
mkdir -p "${WORK}/rootfs"
tar -xzf release/KoboRoot.tgz -C "${WORK}/rootfs"

for f in usr/local/crosskobo/crosskobo usr/local/crosskobo/crosskobo.sh \
         usr/local/crosskobo/start-nickel.sh usr/local/crosskobo/VERSION \
         etc/init.d/on-animator.sh etc/init.d/crosskobo; do
    [ -f "${WORK}/rootfs/${f}" ]
    check $? "${f} present"
done
[ -f "${WORK}/rootfs/usr/local/crosskobo/cacert.pem" ]
check $? "certificates bundled for https"
grep -q 'BEGIN CERTIFICATE' "${WORK}/rootfs/usr/local/crosskobo/cacert.pem"
check $? "the certificate bundle has certificates in it"
# The fetcher is built by scripts/build-fetcher.sh, which needs the network,
# so it is present in CI and absent in a plain local build. When it is
# there it has to be the real thing: a static ARM binary that runs.
if [ -f "${WORK}/rootfs/usr/local/crosskobo/bin/curl" ]; then
    case "$(file -b "${WORK}/rootfs/usr/local/crosskobo/bin/curl")" in
        *ARM*statically*linked*) check 0 "bundled fetcher is a static ARM binary" ;;
        *) check 1 "bundled fetcher is a static ARM binary" ;;
    esac
    if command -v qemu-arm-static >/dev/null 2>&1; then
        qemu-arm-static "${WORK}/rootfs/usr/local/crosskobo/bin/curl" --version \
            >/dev/null 2>&1
        check $? "bundled fetcher runs"
        qemu-arm-static "${WORK}/rootfs/usr/local/crosskobo/bin/curl" --version 2>/dev/null |
            grep -qi 'mbedtls'
        check $? "bundled fetcher has TLS"
        qemu-arm-static "${WORK}/rootfs/usr/local/crosskobo/bin/curl" --version 2>/dev/null |
            grep -qi 'asynchdns'
        check $? "bundled fetcher resolves names without NSS"
    fi
else
    echo "  --   no bundled fetcher in this build (built by CI)"
fi

[ -x "${WORK}/rootfs/usr/local/crosskobo/crosskobo" ]
check $? "binary is executable"
[ -x "${WORK}/rootfs/etc/init.d/on-animator.sh" ]
check $? "boot hook is executable"
[ -L "${WORK}/rootfs/etc/rcS.d/S99crosskobo" ]
check $? "firmware 5 start symlink present"
[ "$(readlink "${WORK}/rootfs/etc/rcS.d/S99crosskobo")" = "../init.d/crosskobo" ]
check $? "symlink points at the init script"
ls "${WORK}/rootfs/usr/local/crosskobo/fonts/"*.ttf >/dev/null 2>&1
check $? "fonts bundled"
ls "${WORK}/rootfs/usr/local/crosskobo/fonts/"LICENSE-*.txt >/dev/null 2>&1
check $? "font licences bundled"

case "$(file -b "${WORK}/rootfs/usr/local/crosskobo/crosskobo")" in
    *"ARM"*"statically linked"*) check 0 "binary is a static ARM executable" ;;
    *) check 1 "binary is a static ARM executable" ;;
esac

# The boot hook must launch the launcher and must not block on it.
grep -q 'crosskobo.sh' "${WORK}/rootfs/etc/init.d/on-animator.sh"
check $? "boot hook starts the launcher"
grep -q 'setsid' "${WORK}/rootfs/etc/init.d/on-animator.sh"
check $? "launcher is detached from the boot hook"
grep -q 'crosskobo-ready' "${WORK}/rootfs/etc/init.d/on-animator.sh"
check $? "boot hook stops animating once CrossKobo has the screen"
grep -q 'killall -q -TERM nickel' "${WORK}/rootfs/usr/local/crosskobo/crosskobo.sh"
check $? "launcher keeps the stock UI from drawing during boot"
grep -q 'crosskobo-allow-nickel' "${WORK}/rootfs/usr/local/crosskobo/start-nickel.sh"
check $? "handover stands the watchdog down"

# The launcher must honour the escape hatch and cap crash loops.
grep -q 'DISABLE' "${WORK}/rootfs/usr/local/crosskobo/crosskobo.sh"
check $? "launcher honours the DISABLE flag"
grep -q 'MAX_CRASHES' "${WORK}/rootfs/usr/local/crosskobo/crosskobo.sh"
check $? "launcher has a crash-loop guard"

# ---------------------------------------------------------------------------
# Dry-run the launcher's safety paths against the extracted payload, with
# the device-specific bits redirected into the sandbox. These are the paths
# that decide whether a bad build can lock somebody out of their Kobo.
echo "launcher safety paths:"
launcher="${WORK}/rootfs/usr/local/crosskobo/crosskobo.sh"

# 1. The user partition never appears: stand down, quietly and quickly.
mkdir -p "${WORK}/case1/rootfs"
cp -r "${WORK}/rootfs/usr" "${WORK}/case1/rootfs/"
sed -e "s#^INSTALL_DIR=.*#INSTALL_DIR=\"${WORK}/case1/rootfs/usr/local/crosskobo\"#" \
    -e "s#^ONBOARD=.*#ONBOARD=\"${WORK}/case1/absent\"#" \
    -e "s#^ALLOW_NICKEL_FLAG=.*#ALLOW_NICKEL_FLAG=\"${WORK}/case1/allow-nickel\"#" \
    -e "s#^DATA_DIR=.*#DATA_DIR=\"${WORK}/case1/absent/.crosskobo\"#" \
    -e 's/while \[ ${i} -lt 120 \]/while [ ${i} -lt 2 ]/' \
    "${launcher}" > "${WORK}/case1/run.sh"
sh "${WORK}/case1/run.sh"
check $? "stands down when the user partition never mounts"
grep -q "standing down" "${WORK}/case1/rootfs/usr/local/crosskobo/boot.log"
check $? "and says so in the boot log"

# 2. The DISABLE flag is present: leave the stock UI alone.
mkdir -p "${WORK}/case2/rootfs" "${WORK}/case2/board/.crosskobo"
cp -r "${WORK}/rootfs/usr" "${WORK}/case2/rootfs/"
: > "${WORK}/case2/board/.crosskobo/DISABLE"
sed -e "s#^INSTALL_DIR=.*#INSTALL_DIR=\"${WORK}/case2/rootfs/usr/local/crosskobo\"#" \
    -e "s#^ONBOARD=.*#ONBOARD=\"${WORK}/case2/board\"#" \
    -e "s#^ALLOW_NICKEL_FLAG=.*#ALLOW_NICKEL_FLAG=\"${WORK}/case2/allow-nickel\"#" \
    -e "s#^DATA_DIR=.*#DATA_DIR=\"${WORK}/case2/board/.crosskobo\"#" \
    -e 's#grep -q " ${ONBOARD} " /proc/mounts#test -d "${ONBOARD}"#g' \
    "${launcher}" > "${WORK}/case2/run.sh"
sh "${WORK}/case2/run.sh"
check $? "honours the DISABLE flag"
grep -q "DISABLE flag present" "${WORK}/case2/board/.crosskobo/boot.log"
check $? "logs to the user partition once it is mounted"

# 3. Three failed starts already recorded: disable itself rather than loop.
mkdir -p "${WORK}/case3/rootfs" "${WORK}/case3/board"
cp -r "${WORK}/rootfs/usr" "${WORK}/case3/rootfs/"
echo 3 > "${WORK}/case3/rootfs/usr/local/crosskobo/crash-count"
sed -e "s#^INSTALL_DIR=.*#INSTALL_DIR=\"${WORK}/case3/rootfs/usr/local/crosskobo\"#" \
    -e "s#^ONBOARD=.*#ONBOARD=\"${WORK}/case3/board\"#" \
    -e "s#^ALLOW_NICKEL_FLAG=.*#ALLOW_NICKEL_FLAG=\"${WORK}/case3/allow-nickel\"#" \
    -e "s#^DATA_DIR=.*#DATA_DIR=\"${WORK}/case3/board/.crosskobo\"#" \
    -e 's#grep -q " ${ONBOARD} " /proc/mounts#test -d "${ONBOARD}"#g' \
    "${launcher}" > "${WORK}/case3/run.sh"
sh "${WORK}/case3/run.sh"
check $? "crash-loop guard exits cleanly"
[ -f "${WORK}/case3/board/.crosskobo/DISABLE" ]
check $? "crash-loop guard writes the DISABLE flag"
[ ! -f "${WORK}/case3/rootfs/usr/local/crosskobo/crash-count" ]
check $? "crash-loop guard clears the counter"

# 4. The handover flag: the firmware re-runs the boot hook when the stock UI
#    starts, and the launcher must stand down rather than take the screen
#    back, or "Return to the Kobo UI" can never complete.
mkdir -p "${WORK}/case4/rootfs" "${WORK}/case4/board" "${WORK}/case4/tmp"
cp -r "${WORK}/rootfs/usr" "${WORK}/case4/rootfs/"
touch "${WORK}/case4/tmp/crosskobo-allow-nickel"
sed -e "s#^INSTALL_DIR=.*#INSTALL_DIR=\"${WORK}/case4/rootfs/usr/local/crosskobo\"#" \
    -e "s#^ONBOARD=.*#ONBOARD=\"${WORK}/case4/board\"#" \
    -e "s#^DATA_DIR=.*#DATA_DIR=\"${WORK}/case4/board/.crosskobo\"#" \
    -e "s#^ALLOW_NICKEL_FLAG=.*#ALLOW_NICKEL_FLAG=\"${WORK}/case4/tmp/crosskobo-allow-nickel\"#" \
    -e 's#grep -q " ${ONBOARD} " /proc/mounts#test -d "${ONBOARD}"#g' \
    "${launcher}" > "${WORK}/case4/run.sh"
sh "${WORK}/case4/run.sh"
check $? "stands down when the handover flag is set"
grep -q "handover flag present" "${WORK}/case4/rootfs/usr/local/crosskobo/boot.log"
check $? "logs why it stood down"
[ -f "${WORK}/case4/tmp/crosskobo-allow-nickel" ]
check $? "leaves the handover flag in place"

# The boot animation must not outlive the thing it is waiting for.
grep -q 'frames} -lt' "${WORK}/rootfs/etc/init.d/on-animator.sh"
check $? "boot hook caps its animation loop"

# ---------------------------------------------------------------------------
echo "install zip:"
ZIP=""
for f in release/CrossKobo-*-install.zip; do
    case "${f}" in
        *fw5*) continue ;;
    esac
    ZIP="${f}"
done
unzip -l "${ZIP}" | grep -q '\.kobo/KoboRoot\.tgz'
check $? "zip places KoboRoot.tgz in .kobo/"
unzip -l "${ZIP}" | grep -q 'READ-ME-FIRST\.txt'
check $? "zip carries instructions"

# ---------------------------------------------------------------------------
echo "catalogues package:"
CAT_ZIP=""
for f in release/Catalogues-*-install.zip; do
    CAT_ZIP="${f}"
done
[ -f "${CAT_ZIP}" ]
check $? "install zip exists"
mkdir -p "${WORK}/cat"
unzip -q -o "${CAT_ZIP}" -d "${WORK}/cat"
[ -f "${WORK}/cat/.adds/nm/catalogues" ]
check $? "ships the NickelMenu entries on the drive"
[ -f "${WORK}/cat/.adds/catalogues/catalogues.txt" ]
check $? "ships the catalogue file on the drive"
grep -q '^Project Gutenberg' "${WORK}/cat/.adds/catalogues/catalogues.txt"
check $? "catalogue file starts with the public catalogues"
if grep -viE '^#|gutenberg|archive\.org|example\.net' "${WORK}/cat/.adds/catalogues/catalogues.txt" |
        grep -q '://'; then
    check 1 "catalogue file ships no other addresses"
else
    check 0 "catalogue file ships no other addresses"
fi
mkdir -p "${WORK}/catroot"
tar -xzf "${WORK}/cat/.kobo/KoboRoot.tgz" -C "${WORK}/catroot"
[ -x "${WORK}/catroot/usr/local/catalogues/catalogues" ]
check $? "carries the program"
case "$(file -b "${WORK}/catroot/usr/local/catalogues/catalogues")" in
    *"ARM"*"statically linked"*) check 0 "program is a static ARM executable" ;;
    *) check 1 "program is a static ARM executable" ;;
esac
if command -v qemu-arm-static >/dev/null 2>&1; then
    qemu-arm-static "${WORK}/catroot/usr/local/catalogues/catalogues" --version | grep -q 'catalogues'
    check $? "program runs"
fi
[ -x "${WORK}/catroot/usr/local/catalogues/start.sh" ]
check $? "carries the launcher"
[ -x "${WORK}/catroot/usr/local/catalogues/uninstall.sh" ]
check $? "carries the remover"
[ -f "${WORK}/catroot/usr/local/catalogues/cacert.pem" ]
check $? "carries the certificates"
[ -f "${WORK}/catroot/usr/local/catalogues/VERSION" ]
check $? "carries its version"
if [ -f release/nickelmenu/KoboRoot.tgz ]; then
    [ -f "${WORK}/catroot/usr/local/Kobo/imageformats/libnm.so" ]
    check $? "bundles NickelMenu"
    [ -f "${WORK}/catroot/mnt/onboard/.adds/nm/doc" ]
    check $? "bundles the NickelMenu documentation"
else
    echo "  --   no NickelMenu fetched in this build (scripts/fetch-nickelmenu.sh)"
fi
if [ -f "${WORK}/catroot/usr/local/catalogues/bin/curl" ]; then
    check 0 "bundles the fetcher"
else
    echo "  --   no bundled fetcher in this build (built by CI)"
fi
# Nothing of CrossKobo, and nothing at boot: this package is the program,
# the menu entries and NickelMenu. No init script, no animation hook, no
# rcS symlink, no framebuffer interface.
if [ -e "${WORK}/catroot/etc" ] || [ -e "${WORK}/catroot/usr/local/crosskobo" ]; then
    check 1 "nothing runs at boot and nothing of CrossKobo is in it"
else
    check 0 "nothing runs at boot and nothing of CrossKobo is in it"
fi
if find "${WORK}/catroot" -type f | grep -qi crosskobo; then
    check 1 "no CrossKobo files in the payload"
else
    check 0 "no CrossKobo files in the payload"
fi
# The menu entries: start the server, then open the browser on it. Every
# launch entry has to wait for the server (cmd_output, not cmd_spawn) or
# the browser opens on nothing.
NM="${WORK}/cat/.adds/nm/catalogues"
grep -q '^menu_item :main :Catalogues :cmd_output' "${NM}"
check $? "menu entry starts the server and waits for it"
grep -q 'chain_always :nickel_browser :modal:http://127.0.0.1:6420/$' "${NM}"
check $? "menu entry opens the Kobo browser (always, not only on success)"
if grep -q 'start.sh --why' "${NM}"; then
    check 1 "menu never dumps the log at the reader"
else
    check 0 "menu never dumps the log at the reader"
fi
# Wi-Fi first: every entry that opens the browser asks the Kobo to connect
# before it does, the way the Kobo itself does for a link.
if [ "$(grep -c 'nickel_wifi :autoconnect' "${NM}")" = "$(grep -c 'nickel_browser' "${NM}")" ]; then
    check 0 "every browser entry asks for Wi-Fi first"
else
    check 1 "every browser entry asks for Wi-Fi first"
fi
grep -q '^menu_item :main :Shelfmark' "${NM}"
check $? "Shelfmark has its own entry"
grep -q 'modal:http://127.0.0.1:6420/shelfmark' "${NM}"
check $? "Shelfmark entry opens its own search page"
grep -q 'nickel_misc :rescan_books' "${NM}"
check $? "a menu entry adds downloads to the library"
grep -q '^menu_item :library :Catalogues' "${NM}"
check $? "the library has an entry too"
if grep -q 'cmd_spawn' "${NM}"; then
    check 1 "no entry opens the browser without waiting for the server"
else
    check 0 "no entry opens the browser without waiting for the server"
fi
if grep -q 'crosskobo' "${NM}"; then
    check 1 "menu entries mention nothing of CrossKobo"
else
    check 0 "menu entries mention nothing of CrossKobo"
fi
# The launcher: start once, wait for the answer, within the menu's timeout.
START="${WORK}/catroot/usr/local/catalogues/start.sh"
grep -q -- '--ping' "${START}"
check $? "launcher checks whether the server is already up"
grep -q -- '--daemon' "${START}"
check $? "launcher starts the server detached"
grep -q 'ifconfig lo' "${START}"
check $? "launcher brings the loopback interface up"
grep -q -- '--idle' "${START}"
check $? "server is started with an idle timeout"
if grep -vE '^[[:space:]]*#' "${START}" | grep -qE 'killall|pkill|nickel|/dev/fb|/dev/input'; then
    check 1 "launcher leaves the stock software alone"
else
    check 0 "launcher leaves the stock software alone"
fi
# The launcher must run to completion in the sandbox with a fake binary
# that answers the ping, and fail cleanly with one that never does.
mkdir -p "${WORK}/start-ok/bin"
cat > "${WORK}/start-ok/bin/catalogues" <<'FAKE'
#!/bin/sh
exit 0
FAKE
chmod 755 "${WORK}/start-ok/bin/catalogues"
sed -e "s#^INSTALL_DIR=.*#INSTALL_DIR=\"${WORK}/start-ok/bin\"#" "${START}" > "${WORK}/start-ok/run.sh"
sh "${WORK}/start-ok/run.sh"
check $? "launcher exits 0 when the server answers"
mkdir -p "${WORK}/start-bad/bin"
cat > "${WORK}/start-bad/bin/catalogues" <<'FAKE'
#!/bin/sh
case "$1" in --ping) exit 1 ;; esac
exit 0
FAKE
chmod 755 "${WORK}/start-bad/bin/catalogues"
sed -e "s#^INSTALL_DIR=.*#INSTALL_DIR=\"${WORK}/start-bad/bin\"#" \
    -e 's/while \[ ${i} -lt 30 \]/while [ ${i} -lt 2 ]/' "${START}" > "${WORK}/start-bad/run.sh"
if sh "${WORK}/start-bad/run.sh"; then
    check 1 "launcher exits 1 when the server never answers"
else
    check 0 "launcher exits 1 when the server never answers"
fi
sed -e "s#^INSTALL_DIR=.*#INSTALL_DIR=\"${WORK}/start-bad/bin\"#" \
    -e "s#^LOG=.*#LOG=\"${WORK}/start-bad/catalogues.log\"#" "${START}" > "${WORK}/start-bad/why.sh"
printf 'line one\nline two\n' > "${WORK}/start-bad/catalogues.log"
sh "${WORK}/start-bad/why.sh" --why | grep -q 'line two'
check $? "launcher --why shows the end of the log"

echo "catalogues uninstaller:"
CAT_UN_ZIP=""
for f in release/Catalogues-*-uninstall.zip; do
    CAT_UN_ZIP="${f}"
done
[ -f "${CAT_UN_ZIP}" ]
check $? "uninstall zip exists"
mkdir -p "${WORK}/catun" "${WORK}/catunroot"
unzip -q -o "${CAT_UN_ZIP}" -d "${WORK}/catun"
tar -xzf "${WORK}/catun/.kobo/KoboRoot.tgz" -C "${WORK}/catunroot"
[ -f "${WORK}/catun/.adds/nm/catalogues" ]
check $? "replaces the menu entries with the remover"
grep -q 'Remove Catalogues' "${WORK}/catun/.adds/nm/catalogues"
check $? "offers Remove Catalogues in the menu"
[ -x "${WORK}/catunroot/usr/local/catalogues/uninstall.sh" ]
check $? "carries a fresh copy of the removal script"
# The whole point: nothing at boot. No init script, no animation hook.
if [ -e "${WORK}/catunroot/etc" ]; then
    check 1 "uninstaller installs nothing that runs at boot"
else
    check 0 "uninstaller installs nothing that runs at boot"
fi
UN="${WORK}/catunroot/usr/local/catalogues/uninstall.sh"
grep -q 'rm -rf /usr/local/catalogues' "${UN}"
check $? "remover takes the install directory"
grep -q 'rm -f /mnt/onboard/.adds/nm/catalogues' "${UN}"
check $? "remover takes the menu entries"
# One path under /mnt, and no wildcard: the catalogue file, the downloads
# and every book stay.
if grep -E '^[^#]*(rm|mv|dd|mkfs)[^#]*/mnt/' "${UN}" |
        grep -vqx 'rm -f /mnt/onboard/.adds/nm/catalogues'; then
    check 1 "remover deletes nothing else under /mnt"
else
    check 0 "remover deletes nothing else under /mnt"
fi
if grep -q 'libnm.so\|imageformats' "${UN}"; then
    check 1 "remover leaves NickelMenu alone"
else
    check 0 "remover leaves NickelMenu alone"
fi

# ---------------------------------------------------------------------------
echo "uninstall payload:"
mkdir -p "${WORK}/unroot"
UNZIP_FILE=""
for f in release/CrossKobo-*-uninstall.zip; do
    case "${f}" in
        *fw5*) continue ;;
    esac
    UNZIP_FILE="${f}"
done
unzip -q -o "${UNZIP_FILE}" -d "${WORK}/unzip"
tar -xzf "${WORK}/unzip/.kobo/KoboRoot.tgz" -C "${WORK}/unroot"
[ -x "${WORK}/unroot/etc/init.d/on-animator.sh" ]
check $? "uninstaller replaces the boot hook"
grep -q 'rm -rf /usr/local/crosskobo' "${WORK}/unroot/etc/init.d/on-animator.sh"
check $? "uninstaller removes the install directory"
grep -q 'S99crosskobo' "${WORK}/unroot/etc/init.d/on-animator.sh"
check $? "uninstaller removes the firmware 5 hook"
grep -q 'pickel showpic' "${WORK}/unroot/etc/init.d/on-animator.sh"
check $? "uninstaller restores the stock animation"
# It must not RUN what it restores. The firmware stops the animation with
# "killall on-animator.sh", which matches on the process name, so a script
# running as "/bin/sh /etc/init.d/on-animator.sh" is called "sh" and
# survives the kill - painting the boot screen over the stock software
# forever. Restoring the file and exiting leaves the next boot to rcS.
if grep -qE '^[^#]*exec[[:space:]]' "${WORK}/unroot/etc/init.d/on-animator.sh"; then
    check 1 "uninstaller does not run the animation it restores"
else
    check 0 "uninstaller does not run the animation it restores"
fi
# The uninstaller may touch exactly one path on the user partition: the
# NickelMenu entry CrossKobo created, which would otherwise point at a
# binary that is gone. Anything else under /mnt - and any recursive or
# wildcard delete - is a bug that could take someone's books with it.
if grep -E '^[^#]*(rm|mv|dd|mkfs)[^#]*/mnt/' \
        "${WORK}/unroot/etc/init.d/on-animator.sh" |
        grep -vqx 'rm -f /mnt/onboard/.adds/nm/crosskobo'; then
    check 1 "uninstaller does not delete anything else under /mnt"
else
    check 0 "uninstaller does not delete anything else under /mnt"
fi

# ---------------------------------------------------------------------------
echo "firmware 5 package:"
FW5=""
for f in release/CrossKobo-*-fw5-install.zip; do
    FW5="${f}"
done
unzip -q -o "${FW5}" -d "${WORK}/fw5"
tar -tf "${WORK}/fw5/.kobo/update.tar" | grep -q '^driver.sh$'
check $? "update.tar carries driver.sh"
tar -tf "${WORK}/fw5/.kobo/update.tar" | grep -q '^crosskobo-install.sh$'
check $? "update.tar carries the install script"
tar -tf "${WORK}/fw5/.kobo/update.tar" | grep -q '^crosskobo.tgz$'
check $? "update.tar carries the payload"

echo
if [ "${fail}" -eq 0 ]; then
    echo "package checks passed"
else
    echo "${fail} package check(s) failed"
    exit 1
fi
