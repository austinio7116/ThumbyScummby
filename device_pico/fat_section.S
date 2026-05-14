/*
 * ThumbyScummby — embed the FAT image into flash.
 *
 * The image is built by tools/build_fat_image.py and lives at
 *   ${CMAKE_CURRENT_BINARY_DIR}/fat_image.bin
 * The CMakeLists adds -I<that-dir> for this file so .incbin can find it.
 *
 * Layout: raw FAT12/16 volume image (boot sector + FAT + root dir +
 * data clusters).  FatFs mounts it through the tsb_flash_disk
 * diskio backend in device_pico/fatfs/tsb_flash_disk.c.
 *
 * .incbin'd into a 4 KB-aligned read-only section so FatFs sector
 * reads land on cleanly aligned flash offsets and so a future
 * cluster-chain XIP resolver can compute absolute flash addresses
 * from cluster numbers.
 */
    .section .rodata.tsb_fat, "a"
    .balign 4096
    .global tsb_fat_image
    .global tsb_fat_image_end
    .global tsb_fat_image_size
tsb_fat_image:
    .incbin "fat_image.bin"
tsb_fat_image_end:

    .balign 4
tsb_fat_image_size:
    .word tsb_fat_image_end - tsb_fat_image
