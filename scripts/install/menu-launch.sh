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
if pkill -0 -f "${INSTALL_DIR}/crosskobo" 2>/dev/null; then
    log "already running"
    exit 0
fi

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
