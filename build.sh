if [ $# -gt 0 ]; then
echo $1 > .version
fi
 
make ARCH=arm CROSS_COMPILE=/home/victor/arm-cortex_a15-linux-gnueabihf-linaro_4.9.2-2014.09/bin/arm-cortex_a15-linux-gnueabihf- zImage -j6
 
cp arch/arm/boot/zImage ~/anykernel/
 
cd ~/anykernel/
 
zipfile="RedKernel-rXXX.zip"
echo "making zip file"
cp zImage zip/kernel/
 
cd zip/
rm -f *.zip
zip -r $zipfile *
rm -f /tmp/*.zip
cp *.zip /tmp
