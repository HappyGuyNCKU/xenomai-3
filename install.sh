#! /bin/bash

./scripts/bootstrap --with-core=cobalt –enable-debug=partial

./configure CFLAGS="-march=armv7-a -mtune=cortex-a8 -mfloat-abi=hard -mfpu=neon -ffast-math" --host=arm-linux-gnueabihf --target=arm-linux-gnueabihf --enable-smp

make DESTDIR=${PWD}/target install

lsblk

until [ "${comfirm}" = "y" ]
do

check=""
until [ "${check}" = "y" ]
do

dev1=""
until [ -n "${dev1}" ]
do


read -p " [] Where is your card reader?[eg] sdx
 -> " reader
dev1=$(lsblk | grep "${reader}"1)
dev2=$(lsblk | grep "${reader}"2)
done

echo
echo ${dev1}
echo ${dev2}
echo 
read -p " [] Is that correct?[y/n]
 -> " check
done

path1=$(echo "$dev1" | grep -o "/media/.*")
path2=$(echo "$dev2" | grep -o "/media/.*")

echo
echo ${path1}
echo ${path2}
echo
read -p " [] Is the path correct?[y/n]
 -> " comfirm
done

echo "Copying"
sudo cp -a target/* ${path2}

echo "Waiting for filesystem sync..."
sync
echo "All done!"
