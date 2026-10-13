#!/bin/sh
# Capture the N900 screen: snap.sh [name]  -> <name>.jpg next to this script
name=${1:-latest}
dir=$(dirname "$0")
v4l2-ctl -d /dev/video0 -c focus_automatic_continuous=0 -c focus_absolute=70
timeout 15 ffmpeg -loglevel error -f v4l2 -input_format mjpeg -video_size 1920x1080 \
	-i /dev/video0 -vf "select=gte(n\,15)" -frames:v 1 -update 1 -y "$dir/raw.jpg" &&
python3 -c "
from PIL import Image
Image.open('$dir/raw.jpg').save('$dir/$name.jpg')"
echo "$dir/$name.jpg $(date +%T)"
