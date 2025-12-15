#!/bin/bash
# Huaicheng <huaicheng@cs.uchicago.edu>
# Please run this script as root.

SYSTEM=`uname -s`

if [[ -f /etc/debian_version ]]; then
	# Includes Ubuntu, Debian
    apt-get install -y gcc pkg-config git libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev
    apt-get install -y libaio-dev libslirp-dev

	# Additional dependencies
	apt-get install -y libnuma-dev
    apt-get install -y ninja-build
    apt-get install -y libnvme-dev liburing-dev libcjson-dev libcap-ng-dev libattr1-dev
else
    echo "pkgdep: unsupported system type ($SYSTEM), please install QEMU depencies manually"
	exit 1
fi

echo "===> Dependency installation ... Done!"
