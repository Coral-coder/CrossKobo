#!/bin/sh
#
# CrossKobo boot hook (Kobo firmware 4.x).
#
# The firmware runs /etc/init.d/on-animator.sh during boot to draw the
# start-up animation. CrossKobo replaces it with this script, which starts
# the CrossKobo launcher and keeps drawing the stock animation until
# CrossKobo has the screen - so boot looks normal, and the stock reading
# software never appears.
#
# This is the mechanism fmon, KFMon and Kobo Start Menu have all used for
# years. To undo it, install the CrossKobo uninstaller, which puts the
# stock animation script back.

INSTALL_DIR="/usr/local/crosskobo"
READY_FLAG="/tmp/crosskobo-ready"

rm -f "${READY_FLAG}"

if [ -x "${INSTALL_DIR}/crosskobo.sh" ]; then
    # setsid detaches the launcher, so it survives this script being killed.
    setsid /bin/sh "${INSTALL_DIR}/crosskobo.sh" >/dev/null 2>&1 &
fi

# ---------------------------------------------------------------------------
# Stock boot animation, drawn until CrossKobo takes over the screen.
# ---------------------------------------------------------------------------
PRODUCT="$(/bin/sh /bin/kobo_config.sh 2>/dev/null)"
[ "${PRODUCT}" != "trilogy" ] && PREFIX="${PRODUCT}-"

COLOR="OFF"
if [ -e "/dev/mmcblk0p6" ]; then
    COLOR="$(ntx_hwconfig -S 1 -p /dev/mmcblk0p6 EPD_Flags CFA 2>/dev/null)"
fi

# Colour panels need the first frame sent as a full update.
PARTIAL_UPDATE=1
if [ "${COLOR}" = "ON" ]; then
    PARTIAL_UPDATE=0
fi

PICKEL="/usr/local/Kobo/pickel"
[ -x "/usr/local/Kobo/pickel-mtk" ] && PICKEL="/usr/local/Kobo/pickel-mtk"

i=0
while [ ! -f "${READY_FLAG}" ]; do
    i=$(( (i + 1) % 11 ))
    image="/etc/images/${PREFIX}on-${i}.raw.gz"
    if [ -s "${image}" ]; then
        zcat "${image}" | ${PICKEL} showpic ${PARTIAL_UPDATE}
        PARTIAL_UPDATE=1
        usleep 250000 2>/dev/null || sleep 1
    else
        sleep 1
    fi
done

# CrossKobo owns the screen now. Stay alive but idle: the firmware kills
# this script when it wants to, and exiting early upsets some rcS versions.
while [ -f "${READY_FLAG}" ]; do
    sleep 10
done
