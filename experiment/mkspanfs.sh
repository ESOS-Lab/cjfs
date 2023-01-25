#!/bin/sh

dev=$1
MNT=$2
domain=$3

	umount ${dev} > /dev/null
	umount ${MNT} > /dev/null
	
	./benchmark/e2fsprogs-1.42.8/build/misc/mke2fs -t ext4 -J size=64 \
	-p ${domain} ${dev} > /dev/null

	mount -t spanfsv2 -o data=ordered ${dev} ${MNT}
