#!/bin/bash

FILE_OP=$1

main()
{
        FILE="cc_${FILE_OP}.dat"
	awk '{ if (NF == 3) print $3 }' $FILE_OP > p${FILE_OP}	
        FILE="fsync_${FILE_LIST}.dat"
	awk '{ if (NF == 3) print $3 }' $FILE_OP > p${FILE_OP}.tmp
	sort -n p${FILE_OP}.tmp > p${FILE_OP}
	sudo rm -f p${FILE_OP}.tmp
        FILE="op_${FILE_OP}.dat"
	awk '{ if (NF == 3) print $3 }' $FILE_OP > p${FILE_OP}.tmp	
	sort -n p${FILE_OP}.tmp > p${FILE_OP}
	sudo rm -f p${FILE_OP}.tmp
}

main
