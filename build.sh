echo ">>> set prerequisites"
export ARCH=arm
export SUBARCH=arm
export CROSS_COMPILE=/home/mani/android/a15-linaro-4.9.1/bin/arm-cortex_a15-linux-gnueabihf-
#make mani_v500_defconfig
#make menuconfig

echo ">>> build zImage"
make -j3

echo ">>> copy zImage to boot/kernel/<"
cp arch/arm/boot/zImage ~/android/boot_aosp/kernel/

echo ">>> copy modules to boot/system/lib/modules<"
find ~/android/v500_aosp -name "*.ko" -exec cp {} ~/android/boot_aosp/system/lib/modules/ \;

zipfile="mani.zip"
echo ">>> build zipfile"
cd ~/android/boot/
rm -f *.zip
zip -9 -r $zipfile *
rm -f /tmp/*.zip
cp *.zip /tmp

echo ">>> ZIP is ready"
