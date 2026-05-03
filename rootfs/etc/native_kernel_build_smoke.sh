export PATH=/bin:/usr/bin:/
echo native-kernel-build-start
mkdir /tmp/kbuild
mkdir /tmp/kbuild/kernel
mkdir /tmp/kbuild/lwip
mkdir /tmp/kbuild/lwip/core
mkdir /tmp/kbuild/lwip/core/ipv4
mkdir /tmp/kbuild/lwip/netif
make -f /src/kernel-build/Makefile BUILD=/tmp/kbuild OUTPUT=/tmp/kernel.elf
echo native-kernel-build-end
