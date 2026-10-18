    .section .rodata
    .balign 4
    .global borg_blob_start
    .type borg_blob_start, %object
borg_blob_start:
    .incbin "libborg.so"
    .global borg_blob_end
    .type borg_blob_end, %object
borg_blob_end:
    .size borg_blob_start, borg_blob_end - borg_blob_start

/* Mark the stack non-executable. */
    .section .note.GNU-stack,"",%progbits
