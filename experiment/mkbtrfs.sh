#!/bin/sh

dev=$1
MNT=$2
domain=$3

	umount ${dev} > /dev/null
	umount ${MNT} > /dev/null
	
	mkfs.btrfs -f ${dev}

	mount -t btrfs ${dev} ${MNT}
