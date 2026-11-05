/* Toolbox icon, embedded so the service can publish it under
 * /data/ghostcontrol without shipping a loose file. 2.0 keeps the asset
 * inside this tree instead of reaching into the ShellUI overlay folder. */
    .section .rodata.gc_icon,"a",@progbits
    .global gc_icon_start
    .global gc_icon_end
gc_icon_start:
    .incbin "assets/icon0.png"
gc_icon_end:
    .section .note.GNU-stack,"",@progbits
