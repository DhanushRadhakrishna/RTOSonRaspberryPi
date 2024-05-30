#
# kernel image packaging for RPi4
#

#export INSTALL_MOD_PATH=$PWD/rfs
export INSTALL_MOD_PATH=$PWD/rfs/usr
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
# pack modules
rm -rf $INSTALL_MOD_PATH 2> /dev/null
mkdir -p $INSTALL_MOD_PATH
make modules_install
cd $INSTALL_MOD_PATH
# remove symlinks that point to files we do not need in root file system
find . -name source | xargs rm
find . -name build | xargs rm
# compress modules
#tar --owner=root --group=root -cvzf ../../modules.tgz .
#tar --owner=root --group=root -cvzf ../../modules.tgz ../usr
cd ../../

# copy kernel and dtb files
export INSTALL_KERN_PATH=$PWD/rfs/boot
rm -rf $INSTALL_KERN_PATH 2> /dev/null
mkdir -p $INSTALL_KERN_PATH/overlays
cd $INSTALL_KERN_PATH
cp ../../arch/arm64/boot/Image kernel8.img
cp ../../arch/arm64/boot/dts/broadcom/*.dtb .
cp ../../arch/arm64/boot/dts/overlays/*.dtb* overlays/
#tar --owner=root --group=root -cvzf ../../boot.tgz .
cd ../../

# package kernel and module files
cd rfs
tar --owner=root --group=root -cvzf ../kernel.tgz .
cd ../
