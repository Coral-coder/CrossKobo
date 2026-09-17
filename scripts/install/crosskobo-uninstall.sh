#!/bin/sh
# Removes CrossKobo from a firmware 5 device.
set -e
rm -f /etc/rcS.d/S99crosskobo
rm -f /etc/init.d/crosskobo
rm -rf /usr/local/crosskobo
# CrossKobo's NickelMenu entries, which would point at a missing binary.
rm -f /mnt/onboard/.adds/nm/crosskobo
sync
