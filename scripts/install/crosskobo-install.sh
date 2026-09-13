#!/bin/sh
# Unpacks CrossKobo into the rootfs on a firmware 5 device.
set -e
ARCHIVE="$1"
tar -C /tmp/updater -xf "${ARCHIVE}" "crosskobo.tgz"
tar -C / -xzf /tmp/updater/crosskobo.tgz
mkdir -p /etc/rcS.d
ln -sf ../init.d/crosskobo /etc/rcS.d/S99crosskobo
chmod 755 /etc/init.d/crosskobo /usr/local/crosskobo/crosskobo.sh \
          /usr/local/crosskobo/start-nickel.sh /usr/local/crosskobo/crosskobo
sync
