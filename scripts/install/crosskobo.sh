#!/bin/sh
#
# CrossKobo launcher.
#
# Started by the boot hook. CrossKobo takes the screen as soon as it can and
# the stock reading software (nickel) never gets to draw: a watchdog stops
# it if the firmware's own start-up scripts launch it behind our back.
#
# Every failure path ends with the stock Kobo UI running, so a bad build
# can never leave the device without a usable interface.
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
READY_FLAG="/tmp/crosskobo-ready"
# start-nickel.sh creates this to tell the watchdog to stand down.
ALLOW_NICKEL_FLAG="/tmp/crosskobo-allow-nickel"
# Until the user partition is mounted the log has to live on the rootfs;
# once it is, logging moves to the drive so it can be read from a computer.
LOG="${INSTALL_DIR}/boot.log"
MAX_CRASHES=3

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "${LOG}"
}

# Keep the boot log from growing forever.
if [ -f "${LOG}" ] && [ "$(wc -c < "${LOG}" 2>/dev/null || echo 0)" -gt 262144 ]; then
    mv -f "${LOG}" "${LOG}.1"
fi

rm -f "${ALLOW_NICKEL_FLAG}"
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

# Move the log somewhere the user can actually read it.
mkdir -p "${DATA_DIR}" 2>/dev/null
if [ -d "${DATA_DIR}" ]; then
    if [ -f "${LOG}" ]; then
        cat "${LOG}" >> "${DATA_DIR}/boot.log" 2>/dev/null && rm -f "${LOG}"
    fi
    LOG="${DATA_DIR}/boot.log"
    log "logging to the user partition"
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
    printf 'CrossKobo disabled itself after %s failed starts.\nDelete this file to try again.\n' \
        "${crashes}" > "${DATA_DIR}/DISABLE"
    rm -f "${CRASH_FILE}"
    exit 0
fi

# ---------------------------------------------------------------------------
# 4. Either take over the boot, or (opt-in) let the stock UI come up first.
#
#    Taking over is the default: CrossKobo starts straight away and a
#    watchdog stops nickel if the firmware's start-up scripts launch it.
#    Creating .crosskobo/START_AFTER_NICKEL asks for the older, more
#    conservative behaviour instead, which is handy when debugging over ssh
#    because the stock system finishes its own start-up first.
# ---------------------------------------------------------------------------
if [ -f "${DATA_DIR}/START_AFTER_NICKEL" ]; then
    log "START_AFTER_NICKEL set; waiting for the stock UI first"
    i=0
    while [ ${i} -lt 90 ]; do
        pkill -0 nickel 2>/dev/null && break
        sleep 1
        i=$((i + 1))
    done
    sleep 3
else
    # Watchdog: for the first minute of uptime, stop nickel whenever it
    # appears. It exits as soon as CrossKobo asks for the stock UI back, or
    # once the boot has clearly settled.
    (
        i=0
        while [ ${i} -lt 300 ]; do
            [ -f "${ALLOW_NICKEL_FLAG}" ] && break
            if pkill -0 nickel 2>/dev/null; then
                killall -q -TERM nickel hindenburg sickel fickel strickel fontickel \
                    adobehost foxitpdf iink 2>/dev/null
            fi
            usleep 200000 2>/dev/null || sleep 1
            i=$((i + 1))
        done
    ) &
    WATCHDOG_PID=$!
    log "boot takeover: watchdog ${WATCHDOG_PID} will keep the stock UI down"
fi

# ---------------------------------------------------------------------------
# 5. Run. The binary handles handing the screen back if the user asks.
# ---------------------------------------------------------------------------
while true; do
    echo $((crashes + 1)) > "${CRASH_FILE}"
    log "starting ${BIN}"
    cd "${INSTALL_DIR}" || exit 0
    LIBC_FATAL_STDERR_=1 "${BIN}" >> "${LOG}" 2>&1
    code=$?
    log "crosskobo exited with ${code}"
    rm -f "${READY_FLAG}"

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
