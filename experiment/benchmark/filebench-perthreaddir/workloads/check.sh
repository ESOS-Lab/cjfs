#! /bin/bash

for i in 1 2 4 6 8 10 20 30 40
do
	cat varmail_${i}.f | head -n 27 | tail -n 1
	cat varmail_${i}.f | head -n 43 | tail -n 1
	cat varmail_${i}.f | head -n 48 | tail -n 1	
done
