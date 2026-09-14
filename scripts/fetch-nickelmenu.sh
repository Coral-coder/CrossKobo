#!/bin/sh
#
# Fetches the NickelMenu release that the Catalogues package bundles, and
# checks it is the one we expect before anything is built on top of it.
#
# NickelMenu (MIT licence, https://github.com/pgaskin/NickelMenu) is what
# puts entries in the stock Kobo menus. It ships inside the Catalogues
# package so there is nothing to install by hand.
#
# Output: release/nickelmenu/KoboRoot.tgz, which scripts/package.sh merges
# into the Catalogues payload when it is there.

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

NM_VERSION="v0.5.4"
NM_URL="https://github.com/pgaskin/NickelMenu/releases/download/${NM_VERSION}/KoboRoot.tgz"
NM_SHA256="450c73c9b096b5ad66f9bff7f0ba9b222ed2a888041649957509cb57123108d0"
OUT="release/nickelmenu/KoboRoot.tgz"

mkdir -p "$(dirname "${OUT}")"
if [ -f "${OUT}" ] && echo "${NM_SHA256}  ${OUT}" | sha256sum -c --quiet 2>/dev/null; then
    printf '%s\n' "${NM_VERSION}" > "$(dirname "${OUT}")/VERSION"
    echo "NickelMenu ${NM_VERSION} already fetched"
    exit 0
fi

echo "fetching NickelMenu ${NM_VERSION}"
curl -sSL --fail --max-time 120 -o "${OUT}.part" "${NM_URL}"
echo "${NM_SHA256}  ${OUT}.part" | sha256sum -c --quiet
mv -f "${OUT}.part" "${OUT}"

# It must be what it says: the plugin, and only files under the paths it is
# known to use.
tar -tzf "${OUT}" | grep -q 'usr/local/Kobo/imageformats/libnm.so$'
if tar -tzf "${OUT}" | grep -v '^\./$' | grep -vE '^\./(usr/local/Kobo/imageformats/|mnt/onboard/\.adds/nm/)'; then
    echo "fetch-nickelmenu: unexpected files in the NickelMenu release" >&2
    exit 1
fi
printf '%s\n' "${NM_VERSION}" > "$(dirname "${OUT}")/VERSION"
echo "NickelMenu ${NM_VERSION} at ${OUT}"
