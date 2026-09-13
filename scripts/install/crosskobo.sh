#!/bin/sh
#
# CrossKobo launcher.
#
# Started by the boot hook while the stock software is still coming up. It
# waits for the system to settle, then takes over the screen. Every failure
# path ends with the stock Kobo UI running, so a bad build can never leave
# the device without a usable interface.
#
# Exit codes from the binary:
#    0  clean exit            -> start the stock UI
#   10  user asked to leave   -> the binary already started the stock UI
#   11  restart CrossKobo     -> loop
#  other                      -> count a crash, start the stock UI

INSTALL_DIR="/usr/local/crosskobo"
ONBOARD="/mnt/onboard"
DATA_DIR="${ONBOARD}/.crosskobo"
BIN="${INSTALL_DIR}/crosskobo"
CRASH_FILE="${INSTALL_DIR}/crash-count"
LOG="${INSTALL_DIR}/boot.log"
MAX_CRASHES=3

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "${LOG}"
}

# Keep the boot log from growing forever.
if [ -f "${LOG}" ] && [ "$(wc -c < "${LOG}" 2>/dev/null || echo 0)" -gt 262144 ]; then
    mv -f "${LOG}" "${LOG}.1"
fi

log "launcher starting (pid $$)"

# ---------------------------------------------------------------------------
# 1. Wait for the user partition. Books, settings and the escape hatch all
#    live there, and it is mounted a little after boot.
# ---------------------------------------------------------------------------
i=0
while [ ${i} -lt 120 ]; do
    if grep -q " ${ONBOARD} " /proc/mounts 2>/dev/null; then
        break
    fi
    usleep 500000 2>/dev/null || sleep 1
    i=$((i + 1))
done
if ! grep -q " ${ONBOARD} " /proc/mounts 2>/dev/null; then
    log "onboard never mounted; standing down"
    exit 0
fi

# ---------------------------------------------------------------------------
# 2. The escape hatch. Creating .crosskobo/DISABLE from a computer stops
#    CrossKobo from starting, without uninstalling anything.
# ---------------------------------------------------------------------------
if [ -f "${DATA_DIR}/DISABLE" ]; then
    log "DISABLE flag present; leaving the stock UI alone"
    exit 0
fi

if [ ! -x "${BIN}" ]; then
    log "binary missing at ${BIN}; standing down"
    exit 0
fi

# ---------------------------------------------------------------------------
# 3. Crash-loop guard. Three failed starts in a row and CrossKobo disables
#    itself, so the device always comes back up in a usable state.
# ---------------------------------------------------------------------------
crashes=0
[ -f "${CRASH_FILE}" ] && crashes="$(cat "${CRASH_FILE}" 2>/dev/null || echo 0)"
case "${crashes}" in
    ''|*[!0-9]*) crashes=0 ;;
esac
if [ "${crashes}" -ge "${MAX_CRASHES}" ]; then
    log "disabled after ${crashes} failed starts; writing DISABLE"
    mkdir -p "${DATA_DIR}"
    printf 'CrossKobo disabled itself after %s failed starts.\nDelete this file to try again.\n' \
        "${crashes}" > "${DATA_DIR}/DISABLE"
    rm -f "${CRASH_FILE}"
    exit 0
fi

# ---------------------------------------------------------------------------
# 4. Wait for the stock UI to come up. It is what sets up Wi-Fi modules,
#    the USB plumbing and the environment we want to inherit; CrossKobo
#    stops it itself once it has taken what it needs.
# ---------------------------------------------------------------------------
i=0
while [ ${i} -lt 90 ]; do
    if pkill -0 nickel 2>/dev/null; then
        log "stock UI is up"
        break
    fi
    sleep 1
    i=$((i + 1))
done

# Give Nickel a moment to finish its own start-up before we stop it, or it
# can leave its database in a sulk.
sleep 3

# ---------------------------------------------------------------------------
# 5. Run. The binary handles stopping and restarting the stock UI itself.
# ---------------------------------------------------------------------------
while true; do
    echo $((crashes + 1)) > "${CRASH_FILE}"
    log "starting ${BIN}"
    cd "${INSTALL_DIR}" || exit 0
    LIBC_FATAL_STDERR_=1 "${BIN}" >> "${LOG}" 2>&1
    code=$?
    log "crosskobo exited with ${code}"

    case "${code}" in
        0)
            rm -f "${CRASH_FILE}"
            /bin/sh "${INSTALL_DIR}/start-nickel.sh" >> "${LOG}" 2>&1
            break
            ;;
        10)
            # The binary started the stock UI on its way out.
            rm -f "${CRASH_FILE}"
            break
            ;;
        11)
            rm -f "${CRASH_FILE}"
            crashes=0
            log "restarting on request"
            continue
            ;;
        *)
            crashes=$((crashes + 1))
            echo "${crashes}" > "${CRASH_FILE}"
            log "unexpected exit; falling back to the stock UI (${crashes}/${MAX_CRASHES})"
            /bin/sh "${INSTALL_DIR}/start-nickel.sh" >> "${LOG}" 2>&1
            break
            ;;
    esac
done

log "launcher finished"
exit 0
