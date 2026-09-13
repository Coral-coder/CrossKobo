#!/bin/sh
#
# Builds the fetcher CrossKobo ships: a static armhf curl with its own TLS
# and its own DNS resolver.
#
# Why bundle one at all - the firmware has neither curl nor wget on every
# device, and CrossKobo replaces the software that would otherwise do the
# fetching. Depending on what happens to be installed makes catalogues work
# on one Kobo and not the next.
#
# Why its own DNS - a statically linked glibc cannot load the NSS modules
# that normally resolve names, so a static curl would fail on exactly the
# thing this is for: an address rather than an IP, from wherever the reader
# happens to be. c-ares does its own DNS from /etc/resolv.conf and sidesteps
# that entirely.
#
# Needs the network, so it runs in CI rather than as part of the ordinary
# build. The output is release/bin/curl, which scripts/package.sh installs
# when it is there and does without when it is not.
set -e

TRIPLE="${CROSSKOBO_TRIPLE:-arm-linux-gnueabihf}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${ROOT}/build-fetcher"
PREFIX="${WORK}/prefix"
OUT="${ROOT}/release/bin"

CARES_VERSION="1.34.4"
MBEDTLS_VERSION="3.6.2"
CURL_VERSION="8.11.1"
CURL_TAG="curl-8_11_1"

# Where the certificates live once installed, compiled into the binary as
# its default so no caller has to pass --cacert.
CA_PATH="/usr/local/crosskobo/cacert.pem"

mkdir -p "${WORK}" "${PREFIX}" "${OUT}"
cd "${WORK}"

fetch() {
    # $1 url, $2 file
    if [ -f "$2" ]; then
        echo "have $2"
        return 0
    fi
    echo "fetching $1"
    curl -fsSL --retry 3 -o "$2.part" "$1"
    mv "$2.part" "$2"
}

# ---------------------------------------------------------------------------
# c-ares: DNS that does not need NSS.
# ---------------------------------------------------------------------------
fetch "https://github.com/c-ares/c-ares/releases/download/v${CARES_VERSION}/c-ares-${CARES_VERSION}.tar.gz" \
    "c-ares.tar.gz"
rm -rf "c-ares-${CARES_VERSION}"
tar -xzf c-ares.tar.gz
cmake -S "c-ares-${CARES_VERSION}" -B build-cares \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/kobo-armhf.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCARES_STATIC=ON -DCARES_SHARED=OFF -DCARES_BUILD_TESTS=OFF \
    -DCARES_BUILD_TOOLS=OFF >/dev/null
cmake --build build-cares -j"$(nproc)" >/dev/null
cmake --install build-cares >/dev/null

# ---------------------------------------------------------------------------
# Mbed-TLS: the TLS itself.
# ---------------------------------------------------------------------------
fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VERSION}/mbedtls-${MBEDTLS_VERSION}.tar.bz2" \
    "mbedtls.tar.bz2"
rm -rf "mbedtls-${MBEDTLS_VERSION}"
tar -xjf mbedtls.tar.bz2
cmake -S "mbedtls-${MBEDTLS_VERSION}" -B build-mbedtls \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/kobo-armhf.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON >/dev/null
cmake --build build-mbedtls -j"$(nproc)" >/dev/null
cmake --install build-mbedtls >/dev/null

# ---------------------------------------------------------------------------
# curl: everything off except what a catalogue needs.
# ---------------------------------------------------------------------------
fetch "https://github.com/curl/curl/releases/download/${CURL_TAG}/curl-${CURL_VERSION}.tar.xz" \
    "curl.tar.xz"
rm -rf "curl-${CURL_VERSION}"
tar -xJf curl.tar.xz
cd "curl-${CURL_VERSION}"
./configure \
    --host="${TRIPLE}" \
    --prefix="${PREFIX}" \
    --with-mbedtls="${PREFIX}" \
    --enable-ares="${PREFIX}" \
    --with-ca-bundle="${CA_PATH}" \
    --disable-shared --enable-static \
    --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet \
    --disable-tftp --disable-pop3 --disable-imap --disable-smtp --disable-gopher \
    --disable-mqtt --disable-smb --disable-manual --disable-docs --disable-libcurl-option \
    --disable-unix-sockets --disable-netrc --disable-progress-meter \
    --without-libpsl --without-libidn2 --without-nghttp2 --without-brotli \
    --without-zstd --without-zlib --without-librtmp \
    LDFLAGS="-static" >/dev/null
# -all-static at make time, not just -static at configure time: the link
# goes through libtool, which understands its own flag and quietly ignores
# the compiler's. Without this the binary comes out dynamically linked
# against the build machine's loader - which is what the emulated check
# caught the first time this ran.
make -j"$(nproc)" LDFLAGS="-all-static" >/dev/null
cd "${WORK}"

case "$(file -b "curl-${CURL_VERSION}/src/curl")" in
    *"statically linked"*) ;;
    *)
        echo "the fetcher did not link statically:" >&2
        file -b "curl-${CURL_VERSION}/src/curl" >&2
        exit 1
        ;;
esac

install -m 755 "curl-${CURL_VERSION}/src/curl" "${OUT}/curl"
"${TRIPLE}-strip" "${OUT}/curl" 2>/dev/null || true

echo "built $(ls -la "${OUT}/curl" | awk '{print $5}') bytes:"
file "${OUT}/curl" || true

# ---------------------------------------------------------------------------
# Prove it works, on ARM, against a real server. A fetcher that cannot do
# this is worse than none: it would look installed and fail every catalogue.
# ---------------------------------------------------------------------------
if command -v qemu-arm-static >/dev/null 2>&1; then
    echo "checking the fetcher under emulation"
    "${ROOT}/scripts/check-fetcher.sh" "${OUT}/curl" "${ROOT}/third_party/cacert.pem"
else
    echo "qemu-arm-static missing, skipping the emulated check"
fi
