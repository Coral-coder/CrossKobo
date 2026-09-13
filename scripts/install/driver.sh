#!/bin/sh
# Kobo firmware 5 update entry point. The OTA updater calls this with the
# path to update.tar and a stage name.
set -e
ARCHIVE="$1"
STAGE="$2"
mkdir -p /tmp/updater
case "${STAGE}" in
    stage1)
        tar -C /tmp/updater -xf "${ARCHIVE}" "crosskobo-install.sh"
        /bin/sh /tmp/updater/crosskobo-install.sh "${ARCHIVE}"
        ;;
    stage2)
        ;;
esac
sync
# Returning non-zero keeps the device from rebooting into recovery.
exit 1
