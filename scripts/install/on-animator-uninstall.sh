#!/bin/sh
#
# CrossKobo uninstaller (Kobo firmware 4.x).
#
# Installed in place of the CrossKobo boot hook. On the next boot it removes
# every CrossKobo file from the rootfs, puts the stock animation script
# back, and then carries on booting normally. It does not touch anything in
# /mnt/onboard, so books, notebooks and settings are left alone.

rm -rf /usr/local/crosskobo
rm -f /etc/init.d/crosskobo
rm -f /etc/rcS.d/S99crosskobo
# The one file CrossKobo adds to the user partition: its NickelMenu entries,
# which would otherwise point at a binary that is no longer there. Books,
# notebooks and settings are left alone.
rm -f /mnt/onboard/.adds/nm/crosskobo

# Restore the stock animation script over ourselves.
cat > /etc/init.d/on-animator.sh <<'STOCK'
#!/bin/sh

PRODUCT="$(/bin/sh /bin/kobo_config.sh)"
[ "${PRODUCT}" != "trilogy" ] && PREFIX="${PRODUCT}-"
COLOR="OFF"
if [ -e "/dev/mmcblk0p6" ] ; then
	COLOR="$(ntx_hwconfig -S 1 -p /dev/mmcblk0p6 EPD_Flags CFA)"
fi

PARTIAL_UPDATE=1
if [ "${COLOR}" == "ON" ] ; then
	PARTIAL_UPDATE=0
fi

i=0
while true ; do
	i=$(( (i + 1) % 11 ))
	image="/etc/images/${PREFIX}on-${i}.raw.gz"
	if [ -s "${image}" ] ; then
		zcat "${image}" | /usr/local/Kobo/pickel showpic ${PARTIAL_UPDATE}
		PARTIAL_UPDATE=1
		usleep 250000
	fi
done
STOCK
chmod 755 /etc/init.d/on-animator.sh
sync

# Stop here. Do NOT run the restored script.
#
# This process was started by rcS in the animation script's place, and the
# firmware stops the animation with "killall on-animator.sh" - which matches
# on the process name. A script run as "/bin/sh /etc/init.d/on-animator.sh"
# is called "sh", so that kill misses it and the boot animation paints over
# the stock software forever. That is exactly what happened.
#
# Exiting now means nothing paints over this boot, and from the next boot rcS
# starts the restored script itself, under its own name, killable exactly as
# the firmware expects.
exit 0
