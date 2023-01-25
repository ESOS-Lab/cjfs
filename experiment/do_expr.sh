#!/bin/bash

source parameter.sh

storage_info()
{
	OUTPUTDIR_DEV=""
	# Identify storage name and set a device result name
	case $1 in
		"/dev/sde") #860PRO
			OUTPUTDIR_DEV=${OUTPUTDIR}/860pro
			;;
		"/dev/nvme1n1") #970pro
			OUTPUTDIR_DEV=${OUTPUTDIR}/970pro
			;;
		"/dev/nvme0n1") #Optane
			OUTPUTDIR_DEV=${OUTPUTDIR}/Intel-900P
			;;
                "ramdisk") #Hardware RAID 0
                        mkdir -p ./ramdisk
                        mount -t ramfs ramfs ./ramdisk
                        OUTPUTDIR_DEV=${OUTPUTDIR}/ramdisk
                        ;;
	esac

	echo $OUTPUTDIR_DEV
}

main()
{
	for BENCHMARK in ${BENCHMARKS[@]}
	do
	
		if [ "$DEBUG" = "debug" ]
		then
			VERSION_PATH="${VERSION_PATH}_${DEBUG}"
			OUTPUTDIR=${VERSION_PATH}/"${DEBUG}_${BENCHMARK}_`date "+%Y%m%d"`_`date "+%H%M"`"
		else
			OUTPUTDIR=${VERSION_PATH}/"${BENCHMARK}_`date "+%Y%m%d"`_`date "+%H%M"`"
		fi 
	
		# Disable ASLR
		echo 0 > /proc/sys/kernel/randomize_va_space

		for dev in ${DEV[@]}
		do
			OUTPUTDIR_DEV=$(storage_info $dev)
			sudo bash run_benchmark.sh ${BENCHMARK} ${OUTPUTDIR_DEV} ${dev} ${domain}
		done
		# Enable ASLR
		echo 2 > /proc/sys/kernel/randomize_va_space
	done
}

main
