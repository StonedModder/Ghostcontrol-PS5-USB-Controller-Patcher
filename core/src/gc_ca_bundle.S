/* The SDK-maintained Mozilla CA bundle is embedded into the payload so HTTPS
 * verification does not depend on a firmware-specific certificate path. */
    .section .rodata.gc_ca_bundle,"a",@progbits
    .global gc_ca_bundle_start
    .global gc_ca_bundle_end
gc_ca_bundle_start:
    .incbin GC_CA_BUNDLE_PATH
gc_ca_bundle_end:
    .byte 0
    .section .note.GNU-stack,"",@progbits
