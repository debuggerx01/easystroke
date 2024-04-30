#!/usr/bin/env bash

apt update
apt install -y make libgtkmm-3.0-dev libdbus-glib-1-dev g++ libboost-serialization-dev xserver-xorg-dev gettext intltool fakeroot

make

if [ -e deb_builder ]; then
    rm -rf deb_builder
fi


mkdir "deb_builder"

cp -r debian deb_builder/DEBIAN
chmod -R 755 deb_builder/DEBIAN

mkdir -p deb_builder/opt/apps/net.sourceforge.thjaeger.easystroke/files/
cp -r dde_package_info/* deb_builder/opt/apps/net.sourceforge.thjaeger.easystroke/

cp easystroke deb_builder/opt/apps/net.sourceforge.thjaeger.easystroke/files/easystroke

mkdir -p deb_builder/opt/apps/net.sourceforge.thjaeger.easystroke/entries/locale
cp -r po/*/ deb_builder/opt/apps/net.sourceforge.thjaeger.easystroke/entries/locale

ARCH="x64"

if [[ $(uname -m) == aarch64 ]]; then
  ARCH="arm64"
  sed -i "s/amd64/$ARCH/g" deb_builder/opt/apps/net.sourceforge.thjaeger.easystroke/info
  sed -i "s/amd64/$ARCH/g" deb_builder/DEBIAN/control
fi

fakeroot dpkg-deb -b deb_builder

mkdir "artifact"

mv deb_builder.deb artifact/net.sourceforge.thjaeger.easystroke_0.6.0-debuggerx-1_"$ARCH".deb