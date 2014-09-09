#!/bin/bash

# Add Colors to unhappiness
green='\033[01;32m'
red='\033[01;31m'
restore='\033[0m'

clear

# Kernel Version
BASE_KVER="mani"
VER="_v18"
KVER=$BASE_KVER$VER

echo -e "${green}"
echo ">>> set prerequisites"
echo -e "${restore}"
export LOCALVERSION="~"`echo $KVER`
export ARCH=arm
export SUBARCH=arm
export CROSS_COMPILE=/home/mani/android/a15-linaro-4.9.2/bin/arm-cortex_a15-linux-gnueabihf-
#make mani_v500_defconfig
#make menuconfig

echo -e "${green}"
echo ">>> build zImage"
echo -e "${restore}"
make -j3

echo -e "${green}"
echo ">>> copy zImage to boot/kernel/<"
echo -e "${restore}"
cp arch/arm/boot/zImage ~/android/boot_aosp/kernel/

echo -e "${green}"
echo ">>> copy modules to boot/system/lib/modules<"
echo -e "${restore}"
find ~/android/v500_aosp -name "*.ko" -exec cp {} ~/android/boot_aosp/system/lib/modules/ \;

zipfile=$KVER
echo -e "${green}"
echo ">>> build zipfile"
echo -e "${restore}"
cd ~/android/boot_aosp/
rm -f *.zip
zip -9 -r $zipfile *
rm -f /tmp/*.zip
cp *.zip /tmp

echo -e "${red}"
echo ">>> ZIP is ready"
echo -e "${restore}"
