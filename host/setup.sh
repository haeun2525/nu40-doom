#!/bin/bash
# A안 준비: doomgeneric 소스를 받아서 NU40DK 플랫폼 레이어를 얹는다.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

if [ ! -d doomgeneric ]; then
  echo "Downloading doomgeneric..."
  git clone --depth 1 https://github.com/ozkl/doomgeneric.git
fi
cp doomgeneric_nu40.c health_shim.c doomgeneric/doomgeneric/
cp Makefile.nu40 doomgeneric/doomgeneric/
# 보드 없이 화면을 미리 보는 도구도 같이 얹는다
cp doomgeneric_capture.c Makefile.capture doomgeneric/doomgeneric/

if [ ! -f doom1.wad ]; then
  echo
  echo "doom1.wad (shareware) not found. id Software allows it to be freely distributed."
  echo "Download it and put it here:  $HERE/doom1.wad"
  echo "  e.g. curl -L -o doom1.wad https://github.com/Akbar30Bill/DOOM_wads/raw/master/doom1.wad"
  echo
fi

cd doomgeneric/doomgeneric
make -f Makefile.nu40
echo
echo "Build complete: $HERE/doomgeneric/doomgeneric/doom_nu40"
echo "Run:  cd $HERE && ./doomgeneric/doomgeneric/doom_nu40 -iwad doom1.wad"
echo "To set the port manually:  NU40_PORT=/dev/cu.usbmodemXXXX ./... "
