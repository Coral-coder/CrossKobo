#!/bin/sh
#
# Starts CrossKobo from a menu inside the stock Kobo software (NickelMenu,
# KFMon, or anything else that can spawn a script) and hands the screen
# back when CrossKobo exits.
#
# Any arguments are passed through, so a menu entry can open straight into
# the catalogue list:
#
#   menu-launch.sh --catalogues
#   menu-launch.sh --catalogue Shelfmark

INSTALL_DIR="/usr/local/crosskobo"
LOG="/mnt/onboard/.crosskobo/menu.log"

mkdir -p "$(dirname "${LOG}")" 2>/dev/null

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "${LOG}"
}

if [ ! -x "${INSTALL_DIR}/crosskobo" ]; then
    log "binary missing; nothing to start"
    exit 0
fi

# Only one at a time: a second launch while CrossKobo owns the screen would
# fight it for the framebuffer.
#
# This cannot ask "pidof crosskobo": the watcher that started us IS a process
# named crosskobo, so that answer is always yes and every tap on the library
# entry did nothing but log "already running". A pid file written by this
# script, checked against /proc, answers the question actually being asked -
# and a stale one from a crash does not wedge it.
LOCK="/tmp/crosskobo-launch.pid"
if [ -f "${LOCK}" ]; then
    running="$(cat "${LOCK}" 2>/dev/null)"
    if [ -n "${running}" ] && [ -d "/proc/${running}" ]; then
        log "already running as ${running}"
        exit 0
    fi
    log "clearing a stale lock from ${running:-nowhere}"
    rm -f "${LOCK}"
fi
echo $$ > "${LOCK}"
trap 'rm -f "${LOCK}"' EXIT

log "starting with: $*"
cd "${INSTALL_DIR}" || exit 0
# --return-to-kobo makes a clean exit start the stock software again, so the
# menu entry behaves like opening an app and closing it.
LIBC_FATAL_STDERR_=1 "${INSTALL_DIR}/crosskobo" --return-to-kobo "$@" >> "${LOG}" 2>&1
code=$?
log "exited with ${code}"

case "${code}" in
    10) : ;;  # CrossKobo started the stock software itself
    *) /bin/sh "${INSTALL_DIR}/start-nickel.sh" >> "${LOG}" 2>&1 ;;
esac

exit 0
