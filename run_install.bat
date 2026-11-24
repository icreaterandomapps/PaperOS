@echo off
REM Boot the ISO and choose I at the PAPEROS boot menu to install to disk.img.
REM After installation, boot the installed disk with run_installed.bat.
"C:\Users\USer\qemu-11.1.0\qemu-system-x86_64.exe" -drive file="%~dp0disk.img",format=raw,if=ide,index=0 -drive file="%~dp0paperos.iso",media=cdrom,if=ide,index=2,readonly=on -boot d -m 64 -vga none -device VGA,vgamem_mb=32 -serial file:"%~dp0serial_install.log" -netdev user,id=n1 -device ne2k_pci,netdev=n1 -monitor tcp:127.0.0.1:4446,server,nowait -no-reboot
