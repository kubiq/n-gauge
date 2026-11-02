// ============================================================================
// netscan.h  —  the WiFi networks around where the car parks (2026-11-02).
//
// Just before the gauge sleeps after a drive, it scans - its own AP down,
// the radio as a station for ~2 s - and adds what it saw to a list kept in
// NVS (netlist.h). The app shows it (GET /api/networks), for the user to
// allow a network, with its password if it has one: where the logs will be
// uploaded from, over WireGuard to the home server.
//
// Only after a wake the car was on in (a CAN frame): the hourly timer wakes
// parked do not scan - the car has not moved since.
// ============================================================================
#pragma once
#include "netlist.h"

// From loop(), right before goToSleep(): scan if the car was on this wake.
// Takes the AP down first (the sleep would next); blocks ~2 s.
void netscan_parked();
// The scan itself, whatever the car did (the bench's 'W'). The AP stays down
// after it until apTask() starts it again, on its next pass.
void netscan_now();
// The list as it is in NVS - any task.
void netscan_load(NetList *out);
