#!/bin/bash -x

phpDir=/monamour/php/bin/

workingDir=`pwd`
currentDir=`cd $(dirname $0); pwd -P`
cd $currentDir

$phpDir/phpize
if [ $? -ne 0 ]; then
    echo -e "phpize FAILED!!!"
    exit 1
fi

#CFLAGS="-O0 -g" ./configure --with-php-config=$phpDir/php-config
#CFLAGS="-O2 -g" ./configure --with-php-config=$phpDir/php-config
./configure --with-php-config=$phpDir/php-config
if [ $? -ne 0 ]; then
    echo -e "configure FAILED!!!"
    exit 2
fi

make
if [ $? -ne 0 ]; then
    echo -e "make FAILED!!!"
    exit 3
fi

cd $workingDir