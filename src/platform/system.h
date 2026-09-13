#pragma once
#include <string>

namespace ck {

// Interaction with the stock Kobo system: stopping and restarting Nickel
// (the stock reader UI), noticing USB, and shutting the radio down.
namespace sys {

bool nickel_running();
// Raises or clears the flag the boot hook watches: while it is set, the
// hook stops drawing the boot animation and leaves the screen to us.
void signal_ready(bool ready);
// Lets the boot watchdog know the stock UI is meant to run from now on.
void allow_nickel();

// Stops the firmware's boot animation.
//
// The animation script draws a frame every quarter second through the
// firmware's own framebuffer tool, and it normally runs until Nickel
// finishes starting and kills it. CrossKobo stops Nickel before that
// happens, so the animation would otherwise keep repainting over the
// interface forever - which is exactly what it did on the first devices
// this ran on. Both firmware generations name the script differently, and
// the drawing tool has to go too.
void stop_boot_animation();
// Clears the launcher's crash counter once CrossKobo has clearly survived
// start-up, so forced power-offs weeks apart cannot add up to a spurious
// self-disable. Three forced power-offs in quick succession still trip it,
// which is the documented way out of a wedged screen.
void clear_crash_count();
// Siphons PLATFORM/PRODUCT/DBUS_SESSION_BUS_ADDRESS etc. out of a running
// Nickel so we can hand them back when restarting it later. Must be called
// before stop_nickel().
void capture_nickel_env();
// Stops Nickel and its helper processes, the way the established Kobo
// launchers do. Returns once the processes are gone (or after a timeout).
void stop_nickel();
// Restarts the stock UI. On firmware 5 this re-runs the system's own
// startup scripts; on firmware 4 it launches hindenburg + nickel directly.
// CrossKobo normally exits right afterwards.
bool start_nickel();

// USB cable state. Used to offer handing over to the stock UI, which owns
// the mass-storage machinery.
bool usb_plugged();

// Wi-Fi is left alone by CrossKobo, but Nickel dislikes finding it up, so
// the handover path shuts it down first.
void wifi_down();

// True when the running process has an (effective) uid of 0. Several
// features degrade gracefully when testing off-device as a normal user.
bool is_root();

int run(const std::string& command);
std::string run_capture(const std::string& command);

}  // namespace sys
}  // namespace ck
