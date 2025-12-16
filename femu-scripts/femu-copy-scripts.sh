#!/bin/bash
# Huaicheng <huaicheng@cs.uchicago.edu>
# Copy necessary scripts for running FEMU

FSD="../femu-scripts"

# Expand run*.sh under $FSD (not under current directory).
shopt -s nullglob
CPL=(pkgdep.sh femu-compile.sh pin.sh ftk)
for f in "$FSD"/run*.sh; do
	CPL+=("$(basename "$f")")
done
shopt -u nullglob

echo ""
echo "==> Copying following FEMU script to current directory:"
for f in "${CPL[@]}"
do
	if [[ ! -e $FSD/$f ]]; then
		echo "Make sure you are under build-femu/ directory!"
		exit
	fi
	cp -r $FSD/$f . && echo "    --> $f"
done

cp ../hw/femu/config_example.json cemu_config.json
echo "    --> cemu_config.json"

echo "Done!"
echo ""

