#
# kernel compile for RPi4
#
source env.sh
make -j$(nproc) Image modules dtbs 
