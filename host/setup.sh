#!/bin/bash
# A안 준비: doomgeneric 소스를 받아서 NU40DK 플랫폼 레이어를 얹는다.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

if [ ! -d doomgeneric ]; then
  echo "doomgeneric 내려받는 중..."
  git clone --depth 1 https://github.com/ozkl/doomgeneric.git
fi
cp doomgeneric_nu40.c health_shim.c doomgeneric/doomgeneric/
cp Makefile.nu40 doomgeneric/doomgeneric/
# 보드 없이 화면을 미리 보는 도구도 같이 얹는다
cp doomgeneric_capture.c Makefile.capture doomgeneric/doomgeneric/

if [ ! -f doom1.wad ]; then
  echo
  echo "doom1.wad(셰어웨어)가 없습니다. id Software 가 무료 배포를 허용한 파일입니다."
  echo "받아서 이 폴더에 두세요:  $HERE/doom1.wad"
  echo "  예) curl -L -o doom1.wad https://github.com/Akbar30Bill/DOOM_wads/raw/master/doom1.wad"
  echo
fi

cd doomgeneric/doomgeneric
make -f Makefile.nu40
echo
echo "빌드 완료: $HERE/doomgeneric/doomgeneric/doom_nu40"
echo "실행:  cd $HERE && ./doomgeneric/doomgeneric/doom_nu40 -iwad doom1.wad"
echo "포트를 직접 지정하려면:  NU40_PORT=/dev/cu.usbmodemXXXX ./... "
