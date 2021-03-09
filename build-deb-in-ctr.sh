#!/bin/bash

set -e


if [[ $# -lt 1 ]]; then
	printf "provide distribution: [debian|ubuntu]\n"
	exit
fi
dist=$1

docker build $BUILD_ARGS -f debian/$dist.dock -t ctcp-builder .
docker run -v `pwd`:/ctcp ctcp-builder
