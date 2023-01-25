#!/bin/bash
make -j 40
make modules -j 40 
sudo make modules_install -j 40
sudo find /lib/modules/5.18.18-CJFS/ -name '*.ko' -exec strip --strip-unneeded {} \;
sudo make install
