#!/bin/sh
#
# Starts the Catalogues server, if it is not already answering, and waits
# until it is. The menu entry runs this and then opens the Kobo's browser on
# the address it serves; that is the whole launch.
#
# Nothing here touches the screen, the input devices or the stock software,
# and nothing runs at boot: the server starts on the first tap and exits by
# itself after an hour without a request.

INSTALL_DIR="/usr/local/catalogues"
PORT=6420
BIN="${INSTALL_DIR}/catalogues"

[ -x "${BIN}" ] || exit 1

if "${BIN}" --ping --port "${PORT}"; then
    exit 0
fi

# Detached from the menu's shell, so it lives on after this script exits.
# The log lives on the drive, next to the catalogue file.
setsid "${BIN}" --port "${PORT}" --idle 60 >/dev/null 2>&1 &

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
