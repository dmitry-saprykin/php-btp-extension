#!/bin/bash

export ZEND_DONT_UNLOAD_MODULES=1
export USE_ZEND_ALLOC=0

sudo valgrind --trace-children=yes --leak-check=full --show-possibly-lost=yes --leak-resolution=med /monamour/php/sbin/php-fpm --fpm-config /monamour/php/etc/php-fpm.conf
