// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — FatFs disk I/O shim backed by the .incbin'd FAT image.
//
// Step 1 of the THUMBYONE_SLOT_PLAN: read-only mount of a baked-in
// FAT image so the engine can open game files via FatFs.  Writes are
// rejected (RES_WRPRT) for now — when MSC + preload pipeline lands
// in plan steps 3+ this file gets replaced by a flash-erase/program
// backed implementation modelled on ThumbyNES/device/nes_flash_disk.c.
//
// The image is .incbin'd by device_pico/fat_section.S; the build
// generates it via tools/build_fat_image.py.

#include "ff.h"
#include "diskio.h"

#include <string.h>

// Linker symbols from fat_section.S — point at the start, end, and
// size of the .incbin'd FAT image.
extern const unsigned char  tsb_fat_image[];
extern const unsigned char  tsb_fat_image_end[];
extern const unsigned int   tsb_fat_image_size;

#define TSB_FAT_SECTOR_SIZE  512u

static inline unsigned int image_size(void) {
    return (unsigned int)(tsb_fat_image_end - tsb_fat_image);
}

static inline LBA_t image_sector_count(void) {
    return (LBA_t)(image_size() / TSB_FAT_SECTOR_SIZE);
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT | STA_NODISK;
    // Treat the .incbin'd region as write-protected — game data is
    // currently baked into firmware.  Real flash backing comes later.
    return STA_PROTECT;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT | STA_NODISK;
    return STA_PROTECT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (count == 0) return RES_OK;
    const LBA_t total = image_sector_count();
    if (sector >= total || sector + count > total) return RES_PARERR;

    // Pure XIP-cached memcpy.  Same pattern as ThumbyNES's
    // nes_flash_disk_read fast path.
    const unsigned char *src = tsb_fat_image + (size_t)sector * TSB_FAT_SECTOR_SIZE;
    memcpy(buff, src, (size_t)count * TSB_FAT_SECTOR_SIZE);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    (void)buff;
    (void)sector;
    (void)count;
    return RES_WRPRT;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0) return RES_PARERR;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(LBA_t *)buff = image_sector_count();
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = TSB_FAT_SECTOR_SIZE;
            return RES_OK;
        case GET_BLOCK_SIZE:
            // Erase block size in *sectors*.  Cosmetic for FatFs (used
            // only by f_mkfs); 4 KB / 512 = 8.
            *(DWORD *)buff = 8;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
