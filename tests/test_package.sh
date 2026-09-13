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

# The launcher must honour the escape hatch and cap crash loops.
grep -q 'DISABLE' "${WORK}/rootfs/usr/local/crosskobo/crosskobo.sh"
check $? "launcher honours the DISABLE flag"
grep -q 'MAX_CRASHES' "${WORK}/rootfs/usr/local/crosskobo/crosskobo.sh"
check $? "launcher has a crash-loop guard"

# ---------------------------------------------------------------------------
echo "install zip:"
ZIP="$(ls release/CrossKobo-*-install.zip | grep -v fw5 | head -1)"
unzip -l "${ZIP}" | grep -q '\.kobo/KoboRoot\.tgz'
check $? "zip places KoboRoot.tgz in .kobo/"
unzip -l "${ZIP}" | grep -q 'READ-ME-FIRST\.txt'
check $? "zip carries instructions"

# ---------------------------------------------------------------------------
echo "uninstall payload:"
mkdir -p "${WORK}/unroot"
UNZIP_FILE="$(ls release/CrossKobo-*-uninstall.zip | grep -v fw5 | head -1)"
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
if grep -E '^[^#]*(rm|mv|dd|mkfs)[^#]*/mnt/' "${WORK}/unroot/etc/init.d/on-animator.sh" \
        >/dev/null 2>&1; then
    check 1 "uninstaller does not delete anything under /mnt"
else
    check 0 "uninstaller does not delete anything under /mnt"
fi

# ---------------------------------------------------------------------------
echo "firmware 5 package:"
FW5="$(ls release/CrossKobo-*-fw5-install.zip | head -1)"
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
