@echo off
REM Boot PAPEROS from the installed IDE disk (disk.img).
"C:\Users\USer\qemu-11.1.0\qemu-system-x86_64.exe" -drive file="%~dp0disk.img",format=raw,if=ide,index=0 -boot c -m 64 -vga none -device VGA,vgamem_mb=32 -serial file:"%~dp0serial_installed.log" -netdev user,id=n1 -device ne2k_pci,netdev=n1 -monitor tcp:127.0.0.1:4447,server,nowait -no-reboot
