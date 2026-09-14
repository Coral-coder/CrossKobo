#!/bin/sh
#
# Catalogues add-on boot hook (Kobo firmware 4.x).
#
# The stock software stays in charge here. All this does is start the
# launcher watcher, then draw the boot animation exactly as the stock script
# does - inline, in this process, so this process keeps the name
# "on-animator.sh" and the firmware's own "killall on-animator.sh" stops it
# when the stock software is up.
#
# That last point is the whole reason this file exists rather than a wrapper
# that execs the stock script: a script run as "/bin/sh .../on-animator.sh"
# is called "sh", the firmware's kill misses it, and the boot animation
# paints over the stock software forever.

INSTALL_DIR="/usr/local/crosskobo"

# The watcher waits for the user partition itself, so fire and forget.
if [ -x "${INSTALL_DIR}/crosskobo" ] && [ -f "${INSTALL_DIR}/crosskobo-watch.init" ]; then
    setsid /bin/sh "${INSTALL_DIR}/crosskobo-watch.init" >/dev/null 2>&1 &
fi

# ---------------------------------------------------------------------------
# The stock animation, verbatim apart from the frame cap.
# ---------------------------------------------------------------------------
PRODUCT="$(/bin/sh /bin/kobo_config.sh 2>/dev/null)"
[ "${PRODUCT}" != "trilogy" ] && PREFIX="${PRODUCT}-"

COLOR="OFF"
if [ -e "/dev/mmcblk0p6" ]; then
    COLOR="$(ntx_hwconfig -S 1 -p /dev/mmcblk0p6 EPD_Flags CFA 2>/dev/null)"
fi

PARTIAL_UPDATE=1
if [ "${COLOR}" = "ON" ]; then
    PARTIAL_UPDATE=0
fi

PICKEL="/usr/local/Kobo/pickel"
[ -x "/usr/local/Kobo/pickel-mtk" ] && PICKEL="/usr/local/Kobo/pickel-mtk"

# The firmware kills this process when the stock software is up. The cap is
# only in case it never does: an animation that outlives the thing it was
# waiting for is the worst failure this file can have.
i=0
frames=0
while [ ${frames} -lt 600 ]; do
    frames=$((frames + 1))
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

exit 0
