#!/bin/sh
#
# Starts the stock Kobo UI. Used when CrossKobo exits or crashes, and by the
# "Return to the Kobo UI" menu item. Mirrors what the firmware's own
# start-up scripts do, for both the firmware 4 and firmware 5 layouts.

PATH="/sbin:/bin:/usr/sbin:/usr/bin:/usr/lib"

# Tell the boot watchdog to stand down: from here on the stock UI is meant
# to be running.
touch /tmp/crosskobo-allow-nickel 2>/dev/null
rm -f /tmp/crosskobo-ready 2>/dev/null

if pkill -0 nickel 2>/dev/null; then
    # Already running: nothing to do.
    exit 0
fi

# Wi-Fi has to be down before Nickel starts, or it gets confused.
killall -q -TERM restore-wifi-async.sh enable-wifi.sh obtain-ip.sh 2>/dev/null
killall -q -TERM udhcpc dhcpcd default.script 2>/dev/null

sync

if [ -e "/etc/init.d/z-nickel-hardware-status" ]; then
    # Firmware 5 and later: re-run the system's own start-up scripts.
    unset LD_LIBRARY_PATH
    /etc/init.d/z-nickel-hardware-status
    sync
    /etc/rc.local
    exit 0
fi

# Firmware 4: recreate the status FIFO and launch the UI the way rcS does.
rm -f /tmp/nickel-hardware-status
mkfifo /tmp/nickel-hardware-status 2>/dev/null

export LD_LIBRARY_PATH="/usr/local/Kobo"
export QT_GSTREAMER_PLAYBIN_AUDIOSINK=alsasink

cd /
/usr/local/Kobo/hindenburg >/dev/null 2>&1 &
LIBC_FATAL_STDERR_=1 /usr/local/Kobo/nickel -platform kobo -skipFontLoad >/dev/null 2>&1 &

if [ -n "${PLATFORM}" ] && [ "${PLATFORM}" != "freescale" ]; then
    udevadm trigger >/dev/null 2>&1 &
fi

exit 0
