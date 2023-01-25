#!/bin/sh

dev=$1
MNT=$2

if [ "${dev}" = "ramdisk" ]
then
	echo ========ramdisk========
	umount ${MNT} > /dev/null

	dd if=/dev/zero of=./${dev}/ext4.image bs=1M count=204800 > /dev/null
	mkfs.ext4 -F -E lazy_journal_init=0,lazy_itable_init=0 ./${dev}/ext4.image
	mount -o loop ./${dev}/ext4.image ${MNT}
else
	umount ${dev} > /dev/null
	umount ${MNT} > /dev/null

#Journal on
        mkfs.ext4 -F -E lazy_journal_init=0,lazy_itable_init=0 ${dev} > /dev/null

#Journal off
#       mkfs.ext4 -O ^has_journal -F -E lazy_journal_init=0,lazy_itable_init=0 ${dev}

#Checksum off
        #mkfs.ext4 -O ^metadata_csum -F -E lazy_journal_init=0,lazy_itable_init=0 ${dev}


	#mount -t ext4 ${dev} ${MNT} > /dev/null
	mount -t ext4 -o nobarrier ${dev} ${MNT} > /dev/null
	sync
fi

