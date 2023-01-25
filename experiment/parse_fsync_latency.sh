#!/bin/sh

dev=$1
nthread=$2
proc=$3

touch temp
touch ${dev}/fsync_${nthread}.dat
while true;
do
	cat /proc/fs/jbd2/${proc:5}-8/fsync > ./temp 
	cat temp >> ${dev}/fsync_${nthread}.dat
	if [ "`cat temp | grep END`" == END ]
	then
		break
	fi
done
sudo rm -rf temp
