// ============================================================================
// usbdisk.cpp  —  see usbdisk.h.
// ============================================================================
#include "usbdisk.h"
#include "logger.h"
#include <Arduino.h>

#if ARDUINO_USB_MODE
// Built for the hardware USB-Serial-JTAG (USBMode=hwcdc): no TinyUSB, no drive.
void usbdisk_init() {}
void usbdisk_task() {}
bool usbdisk_host_connected() { return false; }
bool usbdisk_share() { return false; }
UsbDiskState usbdisk_state() { return USBDISK_IDLE; }
#else

#include "USB.h"
#include "USBMSC.h"
#include "SD_MMC.h"
#include "diskio.h"       // FatFs disk_read/disk_write: several sectors per call

// A GLOBAL on purpose: its constructor adds the MSC interface to the USB
// descriptor, and the core starts USB before setup() runs - created any
// later, the drive would not exist for the host.
static USBMSC msc;

static volatile UsbDiskState state = USBDISK_IDLE;
static volatile bool ejected = false;   // set from the USB task

// The card's FatFs drive number. SD_MMC.readRAW()/writeRAW() take ONE sector
// per call (disk_read(_pdrv, buf, sector, 1)), so a 4 KB request from the PC
// (CONFIG_TINYUSB_MSC_BUFSIZE) was 8 separate card commands; disk_read with
// a count does one multi-block transfer. _pdrv is protected in SDMMCFS: a
// pointer-to-member taken in a derived class reaches it without touching the
// library. Changes on every mount, so read per call.
struct SdmmcDrive : fs::SDMMCFS {
  static uint8_t pdrv() { return SD_MMC.*(&SdmmcDrive::_pdrv); }
};

// TinyUSB hands whole 512-byte blocks (offset 0), up to 8 per call. The bench
// prototype read 3378 sectors on mount with 0 errors one block at a time.
static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (state != USBDISK_SHARED || offset != 0 || bufsize % 512) return -1;
  if (disk_read(SdmmcDrive::pdrv(), (BYTE *)buffer, lba, bufsize / 512) != RES_OK) return -1;
  return bufsize;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  if (state != USBDISK_SHARED || offset != 0 || bufsize % 512) return -1;
  if (disk_write(SdmmcDrive::pdrv(), buffer, lba, bufsize / 512) != RES_OK) return -1;
  return bufsize;
}

// "Eject" in a file manager arrives as a stop with load_eject set.
static bool onStartStop(uint8_t power, bool start, bool loadEject) {
  (void)power;
  if (!start && loadEject) ejected = true;
  return true;
}

void usbdisk_init() {
  msc.vendorID("i30N");
  msc.productID("Gauge SD");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(false);          // empty until "Share card" is tapped
}

bool usbdisk_host_connected() { return (bool)USB; }
UsbDiskState usbdisk_state() { return state; }

bool usbdisk_share() {
  if (state == USBDISK_SHARED || !usbdisk_host_connected() || !logger_card_ok()) return false;
  // Close every file and stop all card access BEFORE the PC can see it.
  logger_suspend();
  // The whole card, not SD_MMC.numSectors(): that is the FAT volume's size
  // (totalBytes / sector), which leaves out the partition offset and FAT's
  // own areas - the PC was told 8192 sectors too few, and Linux truncated
  // the partition ("p1 extends beyond EOD", every share since 2026-10-08):
  // a file in its last 4 MB would have read wrong on the PC.
  const uint32_t sectors = (uint32_t)(SD_MMC.cardSize() / 512);
  if (sectors == 0 || !msc.begin(sectors, 512)) {
    logger_resume();
    return false;
  }
  ejected = false;
  state = USBDISK_SHARED;
  msc.mediaPresent(true);
  Serial.printf("[OK] SD card shared with the PC (%lu sectors)\n", (unsigned long)sectors);
  return true;
}

void usbdisk_task() {
  if (state != USBDISK_SHARED) return;
  // Back when the PC ejects it, or when the cable goes (then the PC never
  // got to finish anything - same remount either way).
  if (!ejected && usbdisk_host_connected()) return;
  msc.mediaPresent(false);
  state = USBDISK_IDLE;
  // Remount, not just carry on: the PC may have written to the FAT, and the
  // firmware's FATFS still holds the old view of it.
  logger_resume();
  Serial.printf("[OK] SD card back from the PC (%s)\n", ejected ? "ejected" : "cable removed");
}

#endif
