#!/bin/bash

# Add Colors to unhappiness
green='\033[01;32m'
red='\033[01;31m'
restore='\033[0m'

clear

# Folders
BASE_DIR="$HOME/Android/kernel"

# Kernel Version
BASE_KVER="aosp_lollipop"
VER="_v1.0"
KVER=$BASE_KVER$VER
DEFCONF="aosp_v500_defconfig"

# Directories & jobs
JOBS=`grep -c "processor" /proc/cpuinfo`
KERNEL_DIR="$BASE_DIR/v500_kernel_aosp"
ZIP_DIR="$BASE_DIR/boot_v500_kernel_aosp"

# start building
echo -e "${green}"
echo ">>> set prerequisites"
echo -e "${restore}"
export LOCALVERSION="-"`echo $KVER`
export ARCH=arm
export SUBARCH=arm
export CROSS_COMPILE="$BASE_DIR/toolchain/arm-cortex_a15-linux-gnueabihf-linaro_4.9.2-2014.10/bin/arm-cortex_a15-linux-gnueabihf-"

echo -e "${green}"
echo ">>> build zImage"
echo -e "${restore}"
make $DEFCONF
make -j$JOBS

echo -e "${green}"
echo ">>> copy zImage"
echo -e "${restore}"
cp arch/arm/boot/zImage $ZIP_DIR/kernel/

echo -e "${green}"
echo ">>> copy modules"
echo -e "${restore}"
rm $ZIP_DIR/system/lib/modules/*
find $KERNEL_DIR -name "*.ko" -exec cp {} $ZIP_DIR/system/lib/modules/ \;

zipfile=$KVER
echo -e "${green}"
echo ">>> build zipfile"
echo -e "${restore}"
cd $ZIP_DIR
rm -f *.zip
zip -9 -r $zipfile.zip *
rm -f /tmp/*.zip
cp *.zip /tmp

echo -e "${red}"
echo ">>> ZIP is ready"
echo -e "${restore}"
