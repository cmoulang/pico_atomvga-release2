This is a DMA version of the pico-atom-VGA firmware.

Files:

    sm.pio - the pio state machines
    atom_if.h - the pio and DMA configuration functions

The implementation allows read/write flags to be set for each 6502 address. 
For example: the following code (1) allows the pico see the Atom's video memory,
(2) see what is written to the PIA register,
(3) provide read write access to the 80 column register area,
and (4) provide 256 byes of RAM at #A00. (NB: for yarrb clear bit 1 of #BFFE to access the RAM at #A00 on the Atom).

    eb_set_perm(FB_ADDR, EB_PERM_WRITE_ONLY, VID_MEM_SIZE);
    eb_set_perm_byte(PIA_ADDR, EB_PERM_WRITE_ONLY);
    eb_set_perm(COL80_BASE, EB_PERM_READ_WRITE, 16);
    eb_set_perm(0xA00, EB_PERM_READ_WRITE, 0x100); 

The demo runs the standard VGA code and also outputs the current content of the text screen and the state of SID register addresses. On linux use the follwoing command to see the output if using a debug probe:

    minicom -b 115200 -o -D /dev/ttyACM0 

Output when running BEEB SID.

    ................................
    ê...................è...........
    ê......................ê.ê......
    ê...................è....è......
    ................................
    êA. 4X4                        .
    êB. BATMAN                     .
    êC. CHRDN                      .
    êD. CNFZN                      .
    êE. COOPDMO                    .
    êf..oneman......................
    êG. SWEET                      .
    êH. SYS4096                    .
    ................................
    ê----------------o--------24:09.
    .a.up.z.down.spc.play.esc.quit..

    Freq Lo      72    Freq Lo      e4    Freq Lo      e0    
    Freq Hi      2e    Freq Hi      5c    Freq Hi       3    
    PW Lo         0    PW Lo         0    PW Lo        50    
    PW Hi         8    PW Hi         8    PW Hi         1    
    Control      41    Control      80    Control      80    
    Attack/Decay 24    Attack/Decay  9    Attack/Decay  7    
    Sustain/Rel   0    Sustain/Rel   9    Sustain/Rel  6f    


