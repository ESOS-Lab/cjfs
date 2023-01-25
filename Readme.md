# CJFS: Concurrent Journaling for Better Scalability
CJFS is the extended version of the journaling module of the ext4 and address the fundamental limitations of the concurrency control of the journaling module of the ext4. This repository contains the whole implementation of the CJFS and whole scripts to reproduce the results in the CJFS paper published at FAST`23

## Content of the repository 
* `5_18_18_barrier/`: kernel including CJFS
* `experiment/`:  scripts for reproducing the results of CJFS
* `figure_set/`: raw data and figure drawn at CJFS

## Environment
* Operating system: CentOS 7.4
* Linux Kernel Version: 5.18.18
* CPU: two Xeon Gold 6230
* RAM: 512GB
* Storage: Samsung 970 Pro (MLC Flash, NVMe)

## The guide to use Kernel Macro
This section briefly explains the kernel macro which can enable/disable for each techniques of CJFS including opportunitistic coalescing, multi-version shadow paging, and compound flush. All the kernel macro introduced at this section is defined at ```include/linux/journal-head.h``` 
* `OP_COALESCING`: The macro to enable opportunistic coalescing.
* `MAX_JH_VERSION`: The macro to designate the number of the version of the shadow page. If this macro is set to larger than one, the PSP macro also should be set and the COMPOUND_FLUSH macro also should be aligned with the MAX_JH_VERSION.
* `COMPOUND_FLUSH`: The macro to enable compound flush. With this macro being set, flush thread issue flush command only if the conditions described at the paper are satisfied. 
* `PSP`: The macro to enable proactive shadow paging. With this macro being set, commit thread creates shadow page for all page cache entries in the transaction.

### Macro set for compiling BarrierFS Kernel
* `OP_COALESCING`: should not be defined.
* `MAX_JH_VERSION`: should be defined as one.
* `COMPOUND_FLUSH`: should not be defined or set to one.
* `PSP`: should not be defined.

### Macro set for compiling CJFS-V3 Kernel
* `OP_COALESCING`: should be defined.
* `MAX_JH_VERSION`: should be defined as three.
* `COMPOUND_FLUSH`: should be defined as three.
* `PSP`: should be defined.

### Macro set for compiling CJFS-V5 Kernel
* `OP_COALESCING`: should be defined.
* `MAX_JH_VERSION`: should be defined as five.
* `COMPOUND_FLUSH`: should be defined as five.
* `PSP`: should be defined.

## Contact Information
* `Joontaek Oh`: na94jun@kaist.ac.kr
* `Seung Won Yoo`: swyoo98@kaist.ac.kr
