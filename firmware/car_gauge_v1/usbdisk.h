// ============================================================================
// usbdisk.h  —  the SD card as a USB drive, on demand, while the gauge runs.
//
// The firmware is a TinyUSB composite device: serial (CDC) and a mass-storage
// drive (MSC). The drive is always in the USB descriptor but EMPTY ("no
// media") until someone taps "Share card" - then logging stops, every file on
// the card is closed, and the PC sees the card. When the PC ejects it (or the
// cable goes), the card is remounted and logging resumes in a new file.
//
// Never two writers: while the PC has the card the firmware does not touch it
// at all - a FAT written from two sides is a corrupted FAT.
// ============================================================================
#pragma once
#include <stdbool.h>

enum UsbDiskState { USBDISK_IDLE, USBDISK_SHARED };

void usbdisk_init();               // in setup(), after the card is mounted
void usbdisk_task();               // in loop(): handles eject / unplug
bool usbdisk_host_connected();     // a PC has enumerated us (bench)
bool usbdisk_share();              // hand the card to the PC; false if not possible
UsbDiskState usbdisk_state();
