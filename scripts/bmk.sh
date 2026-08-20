#!/bin/bash

rm scripts/.tmp/includeShadersMD5
scripts/release.sh

rsync -avz -e ssh release/ enes@192.168.1.250:/home/enes/release
