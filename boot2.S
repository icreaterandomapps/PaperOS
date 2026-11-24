/* ---------------------------------------------------------------
   PAPEROS - Stage 2 bootloader (loaded at 0x7E00)
   - loads the VGA 8x16 font while still in real mode
   - resolution picker (default 800x600)
   - VESA VBE 2.0 linear-framebuffer setup
   - enables A20 so the kernel can use high back buffers
   - loads the kernel from LBA 5 to 0x10000
   - enters 32-bit protected mode
   --------------------------------------------------------------- */
     .code16
     .globl _start2
_start2:
     xorw    %ax, %ax
     movw    %ax, %ds
     movw    %ax, %es

     call    load_font              /* must run before VBE mode set */
     call    print_menu
     call    wait_choice

     movb    $0, 0x501C               /* default: live GUI */
     pushw   %ax                      /* keep the menu choice in AL */
     movw    kernel_sectors + 4, %ax  /* build.py patches this word */
     movw    %ax, 0x501A
     popw    %ax

     /* Boot mode: T = real text-mode shell, I = install to IDE disk,
        L (or any resolution key) = live GUI desktop. */
     cmpb    $'T', %al
     je      boot_tty
     cmpb    $'t', %al
     je      boot_tty
     cmpb    $'I', %al
     je      boot_install
     cmpb    $'i', %al
     je      boot_install
     cmpb    $'L', %al
     je      req_800x600
     cmpb    $'l', %al
     je      req_800x600

     /* Store the requested resolution for the kernel. Stage 2 always sets a
        known-good 800x600 LFB first; the kernel applies custom modes through
        the Bochs VBE extensions after it can serial-log and validate them. */
     cmpb    $'1', %al
     je      req_640x400
     cmpb    $'2', %al
     je      req_640x480
     cmpb    $'4', %al
     je      req_1024x768
     cmpb    $'5', %al
     je      req_1280x720
     cmpb    $'6', %al
     je      req_1360x768
     cmpb    $'7', %al
     je      req_1280x1024
     cmpb    $'8', %al
     je      req_1600x1200
     cmpb    $'9', %al
     je      req_1920x1080
     cmpb    $'A', %al
     je      req_2560x1440
     cmpb    $'a', %al
     je      req_2560x1440
     cmpb    $'B', %al
     je      req_3840x2160
     cmpb    $'b', %al
     je      req_3840x2160
     /* default: option 3 = 800x600 */
req_800x600:
     movw    $800, req_w
     movw    $600, req_h
     jmp     setup_vbe
req_640x400:
     movw    $640, req_w
     movw    $400, req_h
     jmp     setup_vbe
req_640x480:
     movw    $640, req_w
     movw    $480, req_h
     jmp     setup_vbe
req_1024x768:
     movw    $1024, req_w
     movw    $768, req_h
     jmp     setup_vbe
req_1280x720:
     movw    $1280, req_w
     movw    $720, req_h
     jmp     setup_vbe
req_1360x768:
     movw    $1360, req_w
     movw    $768, req_h
     jmp     setup_vbe
req_1280x1024:
     movw    $1280, req_w
     movw    $1024, req_h
     jmp     setup_vbe
req_1600x1200:
     movw    $1600, req_w
     movw    $1200, req_h
     jmp     setup_vbe
req_1920x1080:
     movw    $1920, req_w
     movw    $1080, req_h
     jmp     setup_vbe
req_2560x1440:
     movw    $2560, req_w
     movw    $1440, req_h
     jmp     setup_vbe
req_3840x2160:
     movw    $3840, req_w
     movw    $2160, req_h
     jmp     setup_vbe

/* ---------------------- alternate boot modes --------------------- */
boot_install:
     movb    $2, 0x501C               /* install flag for the kernel */
     jmp     text_mode_setup

boot_tty:
     movb    $1, 0x501C               /* TTY flag for the kernel */

text_mode_setup:
     xorw    %ax, %ax
     movw    %ax, %es                 /* info block writes use ES = 0 */
     movw    $0x0003, %ax             /* real 80x25 text mode */
     int     $0x10
     movw    $80, 0x5000
     movw    $25, 0x5002
     movw    $80, 0x5004
     movb    $8, 0x5006
     movl    $0x000B8000, 0x5008
     movw    $0, 0x500C
     movw    $0, 0x500E
     movw    $80, 0x5014
     movw    $25, 0x5016
     movw    $0, 0x5018
     movl    $0x6000, 0x5010
     jmp     load_kernel

/* -------------------- VESA VBE 2.0 linear base mode -------------- */
setup_vbe:
     movw    $0x0900, %ax
     movw    %ax, %es                 /* mode-info buffer at 0x9000 */
     xorw    %di, %di
     pushw   %es
     movw    $0x4F01, %ax
     movw    $0x0103, %cx             /* 800x600x8 base mode */
     int     $0x10
     popw    %es                      /* some VBE BIOSes clobber ES */
     cmpw    $0x004F, %ax
     jne     classic                  /* VBE not available: safe fallback */

     /* Copy the fields the kernel needs into the info block at 0x5000. */
     movw    %es:0x10, %ax            /* bytes per scanline */
     movw    %ax, 0x5004
     movw    %es:0x12, %ax            /* width */
     movw    %ax, 0x5000
     movw    %es:0x14, %ax            /* height */
     movw    %ax, 0x5002
     movb    %es:0x19, %al            /* bits per pixel */
     movb    %al, 0x5006
     movl    %es:0x28, %eax           /* linear framebuffer address */
     xorw    %cx, %cx
     movw    %cx, %es                 /* ES = 0 */
     movl    %eax, 0x5008
     movw    $0x0103, %ax
     movw    %ax, 0x500C
     movw    $1, 0x500E               /* flag: VBE mode */
     movl    $0x6000, 0x5010          /* 8x16 font buffer */
     movw    req_w, %ax
     movw    %ax, 0x5014
     movw    req_h, %ax
     movw    %ax, 0x5016
     movw    $1, 0x5018               /* requested mode is valid */

     /* Bit 14 requests the linear framebuffer. */
     movw    $0x4F02, %ax
     movw    $0x4103, %bx             /* LFB bit | 800x600x8 */
     int     $0x10
     cmpw    $0x004F, %ax
     jne     classic
     jmp     load_kernel

/* ------------------------ classic mode 13h ----------------------- */
classic:
     xorw    %ax, %ax
     movw    %ax, %es                 /* info block writes use ES = 0 */
     movw    $0x0013, %ax
     int     $0x10
     movw    $320, 0x5000
     movw    $200, 0x5002
     movw    $320, 0x5004
     movb    $8, 0x5006
     movl    $0x000A0000, %eax
     movl    %eax, 0x5008
     movw    $0x0013, 0x500C
     movw    $0, 0x500E
     movl    $0x6000, 0x5010          /* 8x16 font buffer */
     movw    $320, 0x5014
     movw    $200, 0x5016
     movw    $0, 0x5018               /* no custom-mode request in mode 13h */

/* ----------------------- load the kernel ------------------------- */
load_kernel:
     movw    $0x1000, %ax
     movw    %ax, dest_seg            /* ES:BX = 0x10000 */
     xorw    %bx, %bx
     movl    $5, kernel_lba

load_loop:
     /* Hard disks use BIOS LBA reads. Keep the CHS path for floppy/El Torito,
        whose geometry is 18 sectors x 2 heads. */
     cmpb    $0x80, 0x501E
     jb      .Lchs_read
     movw    $1, dap_count
     movw    %bx, dap_off
     movw    dest_seg, %ax
     movw    %ax, dap_seg
     movl    kernel_lba, %eax
     movl    %eax, dap_lba_lo
     movl    $0, dap_lba_hi
     movb    $0x42, %ah
     movb    0x501E, %dl
     movw    $dap, %si
     int     $0x13
     jc      hang2
     jmp     .Lloaded

.Lchs_read:
     movl    kernel_lba, %eax
     xorl    %edx, %edx
     movl    $18, %ecx
     divl    %ecx
     movl    %edx, sector_rem
     movl    %eax, %ecx
     xorl    %edx, %edx
     movl    $2, %esi
     divl    %esi

     movb    %al, %ch
     andl    $0x300, %eax
     shrl    $2, %eax
     movb    sector_rem, %cl
     incb    %cl
     orb     %al, %cl
     movb    %dl, %dh
     movb    0x501E, %dl          /* boot drive saved by stage 1 */

     movw    dest_seg, %ax            /* BIOS may clobber ES, restore it */
     movw    %ax, %es
     movb    $0x02, %ah
     movb    $1, %al
     int     $0x13
     jc      hang2

.Lloaded:
     addw    $0x200, %bx
     jnc     .Lno_wrap
     /* BX is 16-bit: every 128 sectors bump ES by 0x1000 (64 KB). */
     movw    dest_seg, %ax
     addw    $0x1000, %ax
     movw    %ax, dest_seg
     xorw    %bx, %bx
.Lno_wrap:
     incl    kernel_lba
     cmpl    $772, kernel_lba         /* up to 768 kernel sectors */
     jbe     load_loop

     /* -------------------- protected mode -------------------- */
     /* Stage 1 re-enabled IRQs for BIOS disk calls. The kernel is polled, so
        mask them before entering protected mode (no IDT is installed). */
     cli

     /* Fast A20 gate: high back buffers live above 1 MiB. */
     inb     $0x92, %al
     orb     $0x02, %al
     andb    $0xFE, %al
     outb    %al, $0x92

     lgdt    gdtr
     movl    %cr0, %eax
     orl     $1, %eax
     movl    %eax, %cr0

     ljmp    $0x08, $pm_entry

pm_entry:
     .code32
     movw    $0x10, %ax
     movw    %ax, %ds
     movw    %ax, %es
     movw    %ax, %fs
     movw    %ax, %gs
     movw    %ax, %ss
     movl    $0x80000, %esp           /* stack below the high back buffers */
     cld

     /* Keep a pristine copy of the loaded kernel for the installer. The
        running kernel rewrites its own data/BSS, so copying it later would
        produce a corrupt installed image. */
     movzwl  0x501A, %ecx             /* kernel sector count from build.py */
     shll    $9, %ecx                 /* bytes */
     movl    $0x10000, %esi
     movl    $0x1C00000, %edi
     rep movsb

     ljmp    $0x08, $0x10000

     /* Everything below runs in real mode again (called before PM entry). */
     .code16

/* Copy the VGA 8x16 font to 0x6000 before VBE makes VGA RAM unreadable.
   INT 10h AX=1130h BH=6 returns ES:BP = 256 glyphs * 16 bytes. */
load_font:
     pushw   %ds
     pushw   %es
     movw    $0x1130, %ax
     movb    $6, %bh
     int     $0x10
     movw    %es, %ax
     movw    %ax, %ds
     movw    %bp, %si
     movw    $0x0600, %ax             /* buffer segment -> linear 0x6000 */
     movw    %ax, %es
     xorw    %di, %di
     movw    $2048, %cx               /* 4096 bytes = 2048 words */
     cld
     rep movsw
     popw    %es                      /* restore caller's ES, then DS */
     popw    %ds
     ret

hang2:
     hlt
     jmp     hang2

/* ---------------------------- menu ------------------------------- */
print_menu:
     movw    $menu_text, %si
menu_loop:
     lodsb
     testb   %al, %al
     jz      menu_done
     movb    $0x0E, %ah
     movw    $7, %bx
     int     $0x10
     jmp     menu_loop
menu_done:
     ret

wait_choice:
     movw    0x046C, %ax              /* BIOS tick counter */
     movw    %ax, tick_start
key_wait:
     movb    $0x01, %ah
     int     $0x16
     jnz     key_ready
     movw    0x046C, %ax
     subw    tick_start, %ax
     cmpw    $91, %ax                 /* about 5 seconds */
     jb      key_wait
     movb    $'L', %al                /* default = live 800x600 */
     movb    %al, 0x5020
     movb    $0x5A, 0x5021
     ret
key_ready:
     movb    $0x00, %ah
     int     $0x16
     movb    %al, 0x5020
     movb    $0x5A, 0x5021
     ret

menu_text:
     .asciz  "\r\nPAPEROS boot menu\r\n L = live desktop (default)\r\n I = install to IDE disk\r\n T = TTY text mode\r\n Resolutions: 1=640x400 2=640x480 3=800x600\r\n 4=1024x768 5=1280x720 6=1360x768 7=1280x1024\r\n 8=1600x1200 9=1920x1080 A=2560x1440 B=3840x2160\r\nChoice (default L): "

     .align 4
boot_drive: .byte 0
kernel_lba: .long 5
dest_seg:   .word 0x1000
sector_rem: .long 0

     .align 4
dap:
     .byte 0x10, 0
dap_count:  .word 1
dap_off:    .word 0
dap_seg:    .word 0
dap_lba_lo: .long 5
dap_lba_hi: .long 0

kernel_sectors:
            .ascii  "KSEC"          /* build.py patches the following word */
            .word   0
req_w:      .word 800
req_h:      .word 600
tick_start: .word 0

     .align 8
gdt_start:
     .quad 0x0000000000000000
     .quad 0x00CF9A000000FFFF         /* flat 4GB code */
     .quad 0x00CF92000000FFFF         /* flat 4GB data */
gdt_end:

gdtr:
     .word   gdt_end - gdt_start - 1
     .long   gdt_start
