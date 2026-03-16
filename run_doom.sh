#!/bin/bash
(
sleep 3
echo "sendkey slash"
sleep 0.1
echo "sendkey b"
sleep 0.1
echo "sendkey o"
sleep 0.1
echo "sendkey o"
sleep 0.1
echo "sendkey t"
sleep 0.1
echo "sendkey slash"
sleep 0.1
echo "sendkey d"
sleep 0.1
echo "sendkey o"
sleep 0.1
echo "sendkey o"
sleep 0.1
echo "sendkey m"
sleep 0.1
echo "sendkey dot"
sleep 0.1
echo "sendkey e"
sleep 0.1
echo "sendkey l"
sleep 0.1
echo "sendkey f"
sleep 0.1
echo "sendkey ret"
sleep 15
) | qemu-system-x86_64 -M pc -cpu max -m 2G -cdrom orthos.iso -boot d -display none -serial file:serial_doom.log -monitor stdio
