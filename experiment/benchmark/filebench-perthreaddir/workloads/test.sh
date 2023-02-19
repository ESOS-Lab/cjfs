#! /bin/bash

for I in 1 2 4 6 8 10 20 30 40
do
	#sed 's/nfiles=100/nfiles=10000/' -i varmail_$I.f
	sed 's/#flowop fsync/flowop fsync/' -i varmail_$I.f

done
	
