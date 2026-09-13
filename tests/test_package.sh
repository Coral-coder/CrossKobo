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
echo "catalogues add-on:"
ADDON_ZIP=""
for f in release/CrossKobo-catalogues-*-install.zip; do
    ADDON_ZIP="${f}"
done
[ -f "${ADDON_ZIP}" ]
check $? "add-on install zip exists"
mkdir -p "${WORK}/addon"
unzip -q -o "${ADDON_ZIP}" -d "${WORK}/addon"
[ -f "${WORK}/addon/.adds/nm/crosskobo" ]
check $? "add-on ships the NickelMenu entries on the drive"
grep -q 'menu-launch.sh' "${WORK}/addon/.adds/nm/crosskobo"
check $? "menu entries launch through menu-launch.sh"
mkdir -p "${WORK}/addonroot"
tar -xzf "${WORK}/addon/.kobo/KoboRoot.tgz" -C "${WORK}/addonroot"
[ -x "${WORK}/addonroot/usr/local/crosskobo/crosskobo" ]
check $? "add-on carries the binary"
[ -x "${WORK}/addonroot/usr/local/crosskobo/menu-launch.sh" ]
check $? "add-on carries the menu launcher"
[ -x "${WORK}/addonroot/usr/local/crosskobo/crosskobo-watch.init" ]
check $? "add-on carries the watcher init script"
[ -x "${WORK}/addonroot/etc/init.d/crosskobo-watch" ]
check $? "add-on installs the watcher for firmware 5"
[ -L "${WORK}/addonroot/etc/rcS.d/S99crosskobo-watch" ]
check $? "add-on starts the watcher from rcS"
[ -f "${WORK}/addon/CrossKobo Catalogues.txt" ]
check $? "add-on ships the library entry"

# The whole point of this package: the stock software stays in charge. It may
# start a watcher and draw the boot animation, and nothing else.
ADDON_HOOK="${WORK}/addonroot/etc/init.d/on-animator.sh"
grep -q 'crosskobo-watch.init' "${ADDON_HOOK}"
check $? "firmware 4 hook starts the watcher"
grep -q 'showpic' "${ADDON_HOOK}"
check $? "firmware 4 hook draws the stock animation"
grep -q 'frames} -lt' "${ADDON_HOOK}"
check $? "firmware 4 hook caps its animation loop"
# Same trap as the uninstaller: the firmware kills the animation by process
# name, so the hook must draw it inline rather than exec a copy through sh.
if grep -qE '^[^#]*exec[[:space:]]+/bin/sh' "${ADDON_HOOK}"; then
    check 1 "firmware 4 hook keeps the animation in this process"
else
    check 0 "firmware 4 hook keeps the animation in this process"
fi
if grep -rqE 'killall[^#]*nickel|crosskobo\.sh' "${WORK}/addonroot/etc" \
        "${WORK}/addonroot/usr/local/crosskobo/crosskobo-watch.init"; then
    check 1 "add-on never stops the stock software at boot"
else
    check 0 "add-on never stops the stock software at boot"
fi
grep -q -- '--return-to-kobo' "${WORK}/addonroot/usr/local/crosskobo/menu-launch.sh"
check $? "menu launcher hands the screen back on exit"
# The watcher is itself a process named crosskobo, so a "pidof crosskobo"
# guard in the launcher is always true and every tap does nothing. It has to
# ask about its own lock instead.
if grep -vE '^[[:space:]]*#' "${WORK}/addonroot/usr/local/crosskobo/menu-launch.sh" |
        grep -q 'pidof crosskobo'; then
    check 1 "menu launcher does not mistake the watcher for a running app"
else
    check 0 "menu launcher does not mistake the watcher for a running app"
fi
grep -q 'crosskobo-launch.pid' "${WORK}/addonroot/usr/local/crosskobo/menu-launch.sh"
check $? "menu launcher locks on its own pid file"

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
