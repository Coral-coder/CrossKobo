#!/bin/sh
#
# Removes Catalogues. Run from the "Remove Catalogues" menu entry that the
# uninstall package puts in place; nothing runs at boot, so nothing here can
# leave the device stuck on its boot screen.
#
# It takes away exactly what the install package added: the program and its
# menu entries. Books, the Downloads folder, the catalogue file, and
# NickelMenu stay. To remove NickelMenu as well, put an empty file named
# "uninstall" in .adds/nm on the drive and restart.

killall -q catalogues 2>/dev/null
rm -f /mnt/onboard/.adds/nm/catalogues
rm -rf /usr/local/catalogues
sync
exit 0
