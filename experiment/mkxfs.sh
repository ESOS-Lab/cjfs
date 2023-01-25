#!/bin/sh

umount $1 > /dev/null
umount $2 > /dev/null


mkfs.xfs -f -d agsize=419614818304,sunit=256,swidth=1792 -l sunit=256 $1

#mount $1 $2 -o nodev,noatime,attr2,inode64,sunit=256,swidth=1792,noquota
mount $1 $2 -o nodev,noatime,attr2,inode64,sunit=256,swidth=1792,noquota,nobarrier
