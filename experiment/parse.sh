#!/bin/bash

FILE_OP=$1

main()
{
        FILE="cc_${FILE_OP}.dat"
	awk '{ if (NF == 3) print $3 }' $FILE > pcc_${FILE_OP}.dat
        FILE="fsync_${FILE_OP}.dat"
	awk '{ if (NF == 3) print $3 }' $FILE > pfsync_${FILE_OP}.tmp
	sort -n pfsync_${FILE_OP}.tmp > pfsync_${FILE_OP}.dat
	sudo rm -f pfsync_${FILE_OP}.tmp
        FILE="op_${FILE_OP}.dat"
	awk '{ if (NF == 3) print $3 }' $FILE > pop_${FILE_OP}.tmp	
	sort -n pop_${FILE_OP}.tmp > pop_${FILE_OP}.dat
	sudo rm -f pop_${FILE_OP}.tmp
}

main
