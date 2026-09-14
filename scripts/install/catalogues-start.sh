#!/bin/sh
#
# Starts the Catalogues server, if it is not already answering, and waits
# until it is. The menu entry runs this and then opens the Kobo's browser on
# the address it serves; that is the whole launch.
#
# Nothing here touches the screen, the input devices or the stock software,
# and nothing runs at boot: the server starts on the first tap and exits by
# itself after an hour without a request.
#
#   start.sh        start and wait; exit 0 once the server answers
#   start.sh --why  print the end of the log, for the message shown when
#                   it did not

INSTALL_DIR="/usr/local/catalogues"
LOG="/mnt/onboard/.adds/catalogues/catalogues.log"
PORT=6420
BIN="${INSTALL_DIR}/catalogues"

if [ "$1" = "--why" ]; then
    if [ -f "${LOG}" ]; then
        tail -n 6 "${LOG}"
    else
        echo "no log at ${LOG}"
    fi
    [ -x "${BIN}" ] || echo "and ${BIN} is missing"
    exit 0
fi

[ -x "${BIN}" ] || exit 1

if "${BIN}" --ping --port "${PORT}"; then
    exit 0
fi

# The Kobo leaves the loopback interface unconfigured until Wi-Fi is turned
# on, and a server cannot bind to an address the kernel does not have. The
# program sets it up too; this is the belt to its braces.
/sbin/ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1 ||
    ifconfig lo 127.0.0.1 netmask 255.0.0.0 up >/dev/null 2>&1

# It detaches itself: a new session, nothing of this shell kept open.
"${BIN}" --daemon --port "${PORT}" --idle 60

# The menu entry waits at most nine seconds for us; the server is normally
# up in well under one.
i=0
while [ ${i} -lt 30 ]; do
    if "${BIN}" --ping --port "${PORT}"; then
        exit 0
    fi
    usleep 250000 2>/dev/null || sleep 1
    i=$((i + 1))
done
exit 1
