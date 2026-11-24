/* ---------------------------------------------------------------
   PAPEROS - Stage 1 bootloader
   512-byte x86 BIOS boot sector. Loads stage 2 from LBA 1 to 0x7E00
   and jumps to it. Stage 2 shows the resolution picker, sets the video
   mode, loads the kernel, and enters 32-bit protected mode.
   Assembled with GNU as, extracted with objcopy -O binary.
   --------------------------------------------------------------- */
     .code16
     .globl _start
_start:
     cli
     xorw    %ax, %ax
     movw    %ax, %ds
     movw    %ax, %ss
     movw    $0x7C00, %sp
     sti                              /* CD floppy emulation needs IRQs */
     movb    %dl, boot_drive          /* BIOS gives us the boot drive */
     movb    %dl, 0x501E              /* shared with stage 2 and kernel */

     /* Text mode for the stage-2 resolution menu. */
     movw    $0x0003, %ax
     int     $0x10

     /* ---- load stage 2 (4 sectors) from LBA 1 to 0x00007E00 ---- */
     movw    $0x07E0, %ax
     movw    %ax, %es                 /* ES:BX = 0x07E0:0000 = 0x7E00 */
     xorw    %bx, %bx
     movl    $1, stage_lba

     /* Hard disks use BIOS LBA reads; the 18/2 CHS geometry below is for
        the 1.44 MB floppy image and El Torito floppy emulation. */
     cmpb    $0x80, boot_drive
     jb      stage_loop
     movw    $4, dap_count
     movw    $0, dap_off
     movw    $0x07E0, dap_seg
     movl    $1, dap_lba_lo
     movl    $0, dap_lba_hi
     movb    $0x42, %ah
     movb    boot_drive, %dl
     movw    $dap, %si
     int     $0x13
     jc      hang
     ljmp    $0x0000, $0x7E00

stage_loop:
     movl    stage_lba, %eax
     xorl    %edx, %edx
     movl    $18, %ecx
     divl    %ecx                     /* EAX = LBA/18, EDX = LBA%18 */
     movl    %edx, sector_rem
     movl    %eax, %ecx
     xorl    %edx, %edx
     movl    $2, %esi
     divl    %esi                     /* EAX = cylinder, EDX = head */

     movb    %al, %ch
     andl    $0x300, %eax
     shrl    $2, %eax
     movb    sector_rem, %cl
     incb    %cl
     orb     %al, %cl
     movb    %dl, %dh
     movb    boot_drive, %dl

     /* BIOS calls may clobber ES - always restore the destination segment. */
     movw    $0x07E0, %ax
     movw    %ax, %es
     movb    $0x02, %ah
     movb    $1, %al
     int     $0x13
     jc      hang

     addw    $0x200, %bx
     incl    stage_lba
     cmpl    $5, stage_lba            /* load LBA 1,2,3,4 */
     jb      stage_loop

     ljmp    $0x0000, $0x7E00

hang:
     hlt
     jmp     hang

     .align 4
boot_drive: .byte 0
stage_lba:  .long 1
sector_rem: .long 0

     /* BIOS extended-read disk address packet for hard-disk boots. */
dap:
     .byte 0x10, 0
dap_count: .word 1
dap_off:   .word 0
dap_seg:   .word 0
dap_lba_lo: .long 1
dap_lba_hi: .long 0

     /* pad the boot sector and add the signature */
     .fill   510 - (. - _start), 1, 0
     .word   0xAA55
