#!/bin/sh
# Newest frame of the continuous recording (cont/HHMMSS.jpg)
ls -1 "$(dirname "$0")/cont"/*.jpg | tail -1
