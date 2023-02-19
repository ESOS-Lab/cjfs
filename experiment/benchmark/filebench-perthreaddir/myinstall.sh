#!/bin/bash

sudo yum install libtool m4 automake bison flex

libtoolize
aclocal
autoheader
automake --add-missing
autoconf

./configure
make
sudo make install
