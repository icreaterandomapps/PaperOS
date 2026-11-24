@echo off
REM Run PAPEROS live desktop from the generated El Torito ISO.
REM At the boot menu press L for live, I to install, or T for TTY.
"C:\Users\USer\qemu-11.1.0\qemu-system-x86_64.exe" -drive file="%~dp0disk.img",format=raw,if=ide,index=0 -drive file="%~dp0paperos.iso",media=cdrom,if=ide,index=2,readonly=on -boot d -m 64 -vga none -device VGA,vgamem_mb=32 -serial file:"%~dp0serial_iso.log" -netdev user,id=n1 -device ne2k_pci,netdev=n1 -monitor tcp:127.0.0.1:4445,server,nowait -no-reboot
