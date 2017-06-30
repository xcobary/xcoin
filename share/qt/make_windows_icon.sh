#!/bin/bash
# create multiresolution windows icon
ICON_SRC=../../src/qt/res/icons/x.png
ICON_DST=../../src/qt/res/icons/x.ico
convert ${ICON_SRC} -resize 16x16 x-16.png
convert ${ICON_SRC} -resize 32x32 x-32.png
convert ${ICON_SRC} -resize 48x48 x-48.png
convert x-16.png x-32.png x-48.png ${ICON_DST}

