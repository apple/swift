#!/bin/bash

set -ex

sudo apt update

if [ $(grep RELEASE /etc/lsb-release) == "DISTRIB_RELEASE=18.04" ]; then
  sudo apt install -y \
    git ninja-build clang-10 python python-six \
    uuid-dev libicu-dev icu-devtools libbsd-dev \
    libedit-dev libxml2-dev libsqlite3-dev swig \
    libpython-dev libncurses5 libncurses5-dev pkg-config \
    libblocksruntime-dev libcurl4-openssl-dev \
    make systemtap-sdt-dev tzdata rsync wget llvm-10 zip unzip
  sudo ln -s -f /usr/bin/clang-10 /usr/bin/clang
  sudo ln -s -f /usr/bin/clang++-10 /usr/bin/clang++
elif [ $(grep RELEASE /etc/lsb-release) == "DISTRIB_RELEASE=20.04" ]; then
  sudo apt install -y \
    git ninja-build clang python python-six \
    uuid-dev libicu-dev icu-devtools libbsd-dev \
    libedit-dev libxml2-dev libsqlite3-dev swig \
    libpython2-dev libncurses5 libncurses5-dev pkg-config \
    libblocksruntime-dev libcurl4-openssl-dev \
    make systemtap-sdt-dev tzdata rsync wget llvm zip unzip
else
  echo "Unknown Ubuntu version"
  exit 1
fi
sudo apt clean

SOURCE_PATH="$( cd "$(dirname $0)/../../../.." && pwd )" 
SWIFT_PATH=$SOURCE_PATH/swift
cd $SWIFT_PATH

./utils/update-checkout --clone --scheme wasm --skip-repository swift

# Install wasmer

if [ ! -e ~/.wasmer/bin/wasmer ]; then
  curl https://get.wasmer.io -sSfL | sh
fi

cd $SOURCE_PATH

if [ -z $(which cmake) ]; then
  wget -O install_cmake.sh "https://github.com/Kitware/CMake/releases/download/v3.17.2/cmake-3.17.2-Linux-x86_64.sh"
  chmod +x install_cmake.sh
  sudo mkdir -p /opt/cmake
  sudo ./install_cmake.sh --skip-license --prefix=/opt/cmake
  sudo ln -sf /opt/cmake/bin/* /usr/local/bin
fi

cmake --version

# Install sccache

if [ -z $(which sccache) ]; then
  sudo mkdir /opt/sccache && cd /opt/sccache
  wget -O - "https://github.com/mozilla/sccache/releases/download/0.2.13/sccache-0.2.13-x86_64-unknown-linux-musl.tar.gz" | \
    sudo tar xz --strip-components 1
  sudo ln -sf /opt/sccache/sccache /usr/local/bin
fi
