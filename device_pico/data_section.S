/*
 * ThumbyScummby — embed the pre-decrypted MI1 data blob into flash.
 *
 * The blob is built by tools/pack_device.py and lives at
 *   ${CMAKE_CURRENT_BINARY_DIR}/data_blob.bin
 * The CMakeLists adds -I<that-dir> for this file so .incbin can find it.
 *
 * Layout (see tools/pack_device.py):
 *   uint32 magic 'TSDB'
 *   uint32 version (1)
 *   9 x (uint32 offset, uint32 size) entries
 *   <decrypted file bodies>
 */
    .section .rodata.tsb_data, "a"
    .balign 4
    .global tsb_data_blob
    .global tsb_data_blob_end
    .global tsb_data_blob_size
tsb_data_blob:
    .incbin "data_blob.bin"
tsb_data_blob_end:

    .balign 4
tsb_data_blob_size:
    .word tsb_data_blob_end - tsb_data_blob
