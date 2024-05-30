#
# variables for RPi4
#
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
file=$(which ${CROSS_COMPILE}gcc)
if [ ! -x "$file" ];
then 
	echo "ERROR: Cross compiler not found!"; 
fi
