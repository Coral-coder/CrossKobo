#!/bin/sh
#
# Runs the bundled fetcher under emulation and makes it do the one job it
# exists for: an HTTPS request, verified against the CA bundle that ships
# with it, resolving a name rather than an address.
#
# This is the check that matters for a cross-built binary. A fetcher that
# builds but cannot resolve a name - which is exactly how a statically
# linked glibc fails - would look installed and break every catalogue.
set -e

CURL="${1:?usage: check-fetcher.sh <curl> <cacert.pem>}"
CACERT="${2:?usage: check-fetcher.sh <curl> <cacert.pem>}"
URL="${CROSSKOBO_FETCH_CHECK_URL:-https://standardebooks.org/feeds/opds}"

case "$(file -b "${CURL}")" in
    *"ARM"*"statically linked"*) ;;
    *) echo "not a static ARM binary: $(file -b "${CURL}")" >&2; exit 1 ;;
esac

echo "asking ${URL} through the bundled fetcher"
out="$(qemu-arm-static "${CURL}" -sS --max-time 45 --cacert "${CACERT}" \
    -o /dev/null -w '%{http_code} %{ssl_verify_result} %{scheme}' "${URL}")"
echo "answer: ${out}"

status="$(echo "${out}" | cut -d' ' -f1)"
verify="$(echo "${out}" | cut -d' ' -f2)"
scheme="$(echo "${out}" | cut -d' ' -f3)"

[ "${scheme}" = "https" ] || { echo "not https" >&2; exit 1; }
[ "${verify}" = "0" ] || { echo "certificate not verified (${verify})" >&2; exit 1; }
case "${status}" in
    2*|3*) ;;
    *) echo "unexpected status ${status}" >&2; exit 1 ;;
esac

# And the failure it must not get wrong: a certificate that does not verify
# has to be refused, or "verified" means nothing.
if qemu-arm-static "${CURL}" -sS --max-time 30 --cacert /dev/null \
        -o /dev/null "${URL}" 2>/dev/null; then
    echo "accepted a connection with no trust anchors; verification is not working" >&2
    exit 1
fi
echo "and refuses a certificate it cannot verify"

echo "fetcher ok"
