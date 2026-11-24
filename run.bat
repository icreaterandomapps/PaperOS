@echo off
REM Run PAPEROS live desktop from the bootable floppy image.
REM disk.img is attached as primary IDE storage for the file manager.
"C:\Users\USer\qemu-11.1.0\qemu-system-x86_64.exe" -drive file="%~dp0disk.img",format=raw,if=ide,index=0 -drive file="%~dp0os.img",format=raw,if=floppy -boot a -m 64 -vga none -device VGA,vgamem_mb=32 -serial file:"%~dp0serial.log" -netdev user,id=n1 -device ne2k_pci,netdev=n1 -object filter-dump,id=rx,netdev=n1,queue=rx,file="%~dp0net_rx.pcap" -object filter-dump,id=tx,netdev=n1,queue=tx,file="%~dp0net_tx.pcap" -monitor tcp:127.0.0.1:4444,server,nowait -no-reboot
