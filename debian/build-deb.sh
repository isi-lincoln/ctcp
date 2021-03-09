#!/bin/bash

set -e

TARGET=${TARGET:-amd64}
DEBUILD_ARGS=${DEBUILD_ARGS:-""}

rm -f build/ctcp*.build*
rm -f build/ctcp*.change
rm -f build/ctcp*.deb

debuild -e V=1 -e prefix=/usr -e arch=amd64 $DEBUILD_ARGS -aamd64 -i -us -uc -b

mv ../ctcp*.build* build/
mv ../ctcp*.changes build/
mv ../ctcp*.deb build/
