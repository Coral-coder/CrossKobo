#!/bin/sh
#
# Starts the Catalogues server, if it is not already answering, and waits
# until it is. The menu entry runs this and then opens the Kobo's browser on
# the address it serves; that is the whole launch.
#
# It writes to the log on every run - so a tap always leaves a trace, even
# when the program is missing or refuses to start - and it clears a wedged
# instance before starting a fresh one, so a stuck old server can never make
# the page spin forever.
#
#   start.sh        start and wait; exit 0 once the server answers
#   start.sh --why  print the end of the log

INSTALL_DIR="/usr/local/catalogues"
LOG="/mnt/onboard/.adds/catalogues/catalogues.log"
PORT=6420
BIN="${INSTALL_DIR}/catalogues"

mkdir -p "$(dirname "${LOG}")" 2>/dev/null
log() { echo "[$(date '+%H:%M:%S')] start.sh: $*" >> "${LOG}" 2>/dev/null; }

if [ "$1" = "--why" ]; then
    if [ -f "${LOG}" ]; then tail -n 8 "${LOG}"; else echo "no log at ${LOG}"; fi
    [ -x "${BIN}" ] || echo "and ${BIN} is missing"
    exit 0
fi

if [ ! -x "${BIN}" ]; then
    log "the program is not installed at ${BIN} - the update (.kobo/KoboRoot.tgz) did not apply"
    exit 1
fi

# Already healthy? Leave it. --ping is bounded (2s), so a wedged server does
# not hang us here.
if "${BIN}" --ping --port "${PORT}"; then
    log "already answering on ${PORT}"
    exit 0
fi

# Not answering: clear any stale/wedged instance so a fresh one owns the port.
if pkill -x catalogues >/dev/null 2>&1; then
    log "cleared a stale server"
    sleep 1
fi

# The Kobo leaves loopback unconfigured until Wi-Fi is on; the program sets
# it up too, this is the belt to its braces.
/sbin/ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 ||
    ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1

log "starting the server on ${PORT}"
"${BIN}" --daemon --port "${PORT}" --idle 60

i=0
while [ ${i} -lt 30 ]; do
    if "${BIN}" --ping --port "${PORT}"; then
        log "up after $((i / 4)) s"
        exit 0
    fi
    usleep 250000 2>/dev/null || sleep 1
    i=$((i + 1))
done
log "server did not come up - see the lines above"
exit 1
