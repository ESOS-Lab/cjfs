#!/bin/sh

dev=$1
nthread=$2
proc=$3


touch temp
touch ${dev}/op_${nthread}.dat
while true;
do
	cat /proc/fs/jbd2/${proc:5}-8/op > ./temp 
	cat temp >> ${dev}/op_${nthread}.dat
	if [ "`cat temp | grep END`" == END ]
	then
		break
	fi
done
sudo rm -rf temp

#touch temp
#touch ${dev}/fsync_${nthread}.dat
#while true;
#do
#	cat /proc/fs/jbd2/${proc:5}-8/fsync > ./temp 
#	cat temp >> ${dev}/fsync_${nthread}.dat
#	if [ "`cat temp | grep END`" == END ]
#	then
#		break
#	fi
#done
#sudo rm -rf temp

touch temp
touch ${dev}/cc_${nthread}.dat
while true;
do
	cat /proc/fs/jbd2/${proc:5}-8/cc > ./temp 
	cat temp >> ${dev}/cc_${nthread}.dat
	if [ "`cat temp | grep END`" == END ]
	then
		break
	fi
done
