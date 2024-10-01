This is an experimental DMA version of the pico-atom-VGA firmware.

A lookup table contains the read/write flags for each 6502 address. For example:

    // initially set all addresses to NO_ACCESS
    eb_set_perm(0, EB_PERM_NO_ACCESS, 0x10000);
    // set the video memory to WRITE_ONLY
    eb_set_perm(FB_ADDR, EB_PERM_WRITE_ONLY, VID_MEM_SIZE);
    // allow read/write access to the GODIL register addresses
    eb_set_perm(COL80_BASE, EB_PERM_READ_WRITE, 16);
    // allow write access to PIA address
    eb_set_perm_byte(PIA_ADDR, EB_PERM_WRITE_ONLY);

This works reliably at 1MHz and 2MHz on a 65C02

In the initial version a whole CPU was used to handle the interface to the PIO - it now calculates pi and outputs it's latests guess to the serial port once a second or so.
