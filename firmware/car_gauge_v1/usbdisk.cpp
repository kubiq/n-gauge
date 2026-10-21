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
void usbdisk_unshare() {}
UsbDiskState usbdisk_state() { return USBDISK_IDLE; }
#else

#include "USB.h"
#include "esp32-hal-tinyusb.h"
#include "SD_MMC.h"
#include "diskio.h"       // FatFs disk_read: several sectors per call

// ---- the drive, without the core's USBMSC class ------------------------------
// USBMSC answers a READ with "busy, ask again" (TinyUSB's 0) while the
// medium is absent, and TinyUSB then asks again forever. A read from the PC
// arriving after an eject had cleared the medium therefore never finished;
// the retry spin starved loop() into the 8 s watchdog, the pod restarted
// with the medium still absent, the PC's retried read spun again - and the
// PC reset the device every ~9 s until its SCSI layer gave up (2026-10-12
// and -13, with and without PC writes). So the callbacks are ours, and with
// nothing shared every command fails at once with MEDIUM NOT PRESENT, which
// is what a card reader with no card says.

static volatile UsbDiskState state = USBDISK_IDLE;
static volatile bool ejected = false;   // set from the USB task
static bool stopped = false;            // "Stop sharing" tapped on the gauge
static volatile uint32_t sectors = 0;   // the card's size while shared

static void notPresent(uint8_t lun) { tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00); }

static uint16_t loadDescriptor(uint8_t *dst, uint8_t *itf) {
  const uint8_t str = tinyusb_add_string_descriptor("Gauge SD");
  const uint8_t ep = tinyusb_get_free_duplex_endpoint();
  TU_VERIFY(ep != 0);
  const uint8_t d[TUD_MSC_DESC_LEN] = {TUD_MSC_DESCRIPTOR(*itf, str, ep, (uint8_t)(0x80 | ep), CFG_TUD_ENDPOINT_SIZE)};
  *itf += 1;
  memcpy(dst, d, TUD_MSC_DESC_LEN);
  return TUD_MSC_DESC_LEN;
}

// Constructed before setup() on purpose: the core starts USB before setup()
// runs, and an interface added any later would not exist for the host.
static struct MscInterface {
  MscInterface() { tinyusb_enable_interface(USB_INTERFACE_MSC, TUD_MSC_DESC_LEN, loadDescriptor); }
} mscInterface;

// The card's FatFs drive number. SD_MMC.readRAW()/writeRAW() take ONE sector
// per call (disk_read(_pdrv, buf, sector, 1)), so a 4 KB request from the PC
// (CONFIG_TINYUSB_MSC_BUFSIZE) was 8 separate card commands; disk_read with
// a count does one multi-block transfer. _pdrv is protected in SDMMCFS: a
// pointer-to-member taken in a derived class reaches it without touching the
// library. Changes on every mount, so read per call.
struct SdmmcDrive : fs::SDMMCFS {
  static uint8_t pdrv() { return SD_MMC.*(&SdmmcDrive::_pdrv); }
};

extern "C" {

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor[8], uint8_t product[16], uint8_t rev[4]) {
  (void)lun;
  memcpy(vendor, "i30N    ", 8);
  memcpy(product, "Gauge SD        ", 16);
  memcpy(rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  if (state == USBDISK_SHARED) return true;
  notPresent(lun);
  return false;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *count, uint16_t *size) {
  (void)lun;
  *count = state == USBDISK_SHARED ? sectors : 0;
  *size = 512;
}

// TinyUSB hands whole 512-byte blocks (offset 0), up to 8 per call. The bench
// prototype read 3378 sectors on mount with 0 errors one block at a time.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (state != USBDISK_SHARED) { notPresent(lun); return -1; }
  if (offset != 0 || bufsize % 512 ||
      disk_read(SdmmcDrive::pdrv(), (BYTE *)buffer, lba, bufsize / 512) != RES_OK) {
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);   // unrecovered read error
    return -1;
  }
  return bufsize;
}

// The drive is READ-ONLY (2026-10-12). Even with nothing copied, the PC
// writes - the FAT "in use" flag, access dates of the files a file manager
// previews - and there is no reason to let it: deleting logs is done with
// the card in a reader. TinyUSB refuses writes to a write-protected LUN
// before calling this; it answers the same anyway.
bool tud_msc_is_writable_cb(uint8_t lun) { (void)lun; return false; }

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  (void)lba; (void)offset; (void)buffer; (void)bufsize;
  tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);    // write protected
  return -1;
}

// "Eject" in a file manager arrives as a stop with load_eject set.
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power, bool start, bool loadEject) {
  (void)lun; (void)power;
  if (!start && loadEject) ejected = true;
  return true;
}

// Everything TinyUSB does not handle itself: allow "prevent removal" (the
// PC asks before mounting), refuse the rest.
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const cmd[16], void *buffer, uint16_t bufsize) {
  (void)buffer; (void)bufsize;
  if (cmd[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL) return 0;
  tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
  return -1;
}

}  // extern "C"

// Nothing left to set up: the interface is added before setup() (above), and
// the drive is empty until usbdisk_share().
void usbdisk_init() {}

bool usbdisk_host_connected() { return (bool)USB; }
UsbDiskState usbdisk_state() { return state; }

bool usbdisk_share() {
  if (state == USBDISK_SHARED || !usbdisk_host_connected() || !logger_card_ok()) return false;
  // Close every file and stop all card access BEFORE the PC can see it.
  if (!logger_suspend()) return false;
  // The whole card, not SD_MMC.numSectors(): that is the FAT volume's size
  // (totalBytes / sector), which leaves out the partition offset and FAT's
  // own areas - the PC was told 8192 sectors too few, and Linux truncated
  // the partition ("p1 extends beyond EOD", every share since 2026-10-08):
  // a file in its last 4 MB would have read wrong on the PC.
  sectors = (uint32_t)(SD_MMC.cardSize() / 512);
  if (sectors == 0) {
    logger_resume();
    return false;
  }
  ejected = false;
  stopped = false;
  state = USBDISK_SHARED;
  Serial.printf("[OK] SD card shared with the PC (%lu sectors)\n", (unsigned long)sectors);
  return true;
}

void usbdisk_task() {
  if (state != USBDISK_SHARED) return;
  // Back when the PC ejects it, when "Stop sharing" is tapped on the gauge,
  // or when the cable goes.
  if (!ejected && !stopped && usbdisk_host_connected()) return;
  state = USBDISK_IDLE;
  logger_resume();
  Serial.printf("[OK] SD card back from the PC (%s)\n",
                ejected ? "ejected" : stopped ? "stopped on the gauge" : "cable removed");
}

// Safe from the gauge's side since the drive is read-only and answers
// MEDIUM NOT PRESENT once taken back (both 2026-10-13): the PC can lose
// nothing but a copy still running, which it reports as failed. Before,
// with the PC writing, only its own eject could end a share.
void usbdisk_unshare() {
  if (state == USBDISK_SHARED) stopped = true;
}

#endif
