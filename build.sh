#!/bin/bash

sudo docker build -f build.dock -t ctcp .
sudo docker run -v $PWD/bin:/tmp/bin ctcp /bin/sh -c "cp -r /ctcp/bin/* /tmp/bin" 
