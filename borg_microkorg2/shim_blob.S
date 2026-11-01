/* The armhf object the unit's switch writes to /tmp and preloads into
 * microkorgd, carried inside the unit rather than shipped beside it.
 * build-mk2.sh compiles mk2_shim.c to libborg.so in this directory, with the
 * platform's own cross compiler, immediately before make runs; the include
 * below needs that file to exist. */
    .section .rodata
    .balign 4
    .global shim_blob_start
shim_blob_start:
    .incbin "libborg.so"
.global shim_blob_end
shim_blob_end:
    .section .note.GNU-stack,"",%progbits
