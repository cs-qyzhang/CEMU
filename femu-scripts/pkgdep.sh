#!/bin/bash
# Huaicheng <huaicheng@cs.uchicago.edu>
# Please run this script as root.

SYSTEM=`uname -s`

if [[ -f /etc/debian_version ]]; then
	# Includes Ubuntu, Debian
    apt-get install -y gcc pkg-config git libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev
    apt-get install -y libaio-dev libslirp-dev
    apt-get install -y flex bison libelf-dev libssl-dev cmake 
    apt-get install -y libbpf-dev liblz4-dev libboost-dev 
    apt-get install -y libboost-program-options-dev libboost-filesystem-dev
	# Additional dependencies
	apt-get install -y libnuma-dev
    apt-get install -y ninja-build clang
    apt-get install -y libnvme-dev liburing-dev libcjson-dev libcap-ng-dev libattr1-dev
else
    echo "pkgdep: unsupported system type ($SYSTEM), please install QEMU depencies manually"
	exit 1
fi

echo "===> Dependency installation ... Done!"
