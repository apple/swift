#!/bin/bash
set -ex
DESTINATION_TOOLCHAIN=$1
WASI_SYSROOT_PATH=$2
SOURCE_PATH="$(cd "$(dirname $0)/../../.." && pwd)"

BUILD_DIR="$SOURCE_PATH/target-build/xctest-wasi-wasm32"

mkdir -p $BUILD_DIR
cd $BUILD_DIR

cmake -G Ninja \
  -DCMAKE_SYSROOT="$WASI_SYSROOT_PATH" \
  -DCMAKE_Swift_COMPILER="$DESTINATION_TOOLCHAIN/usr/bin/swiftc" \
  -DCMAKE_STAGING_PREFIX="$DESTINATION_TOOLCHAIN/usr" \
  -DCMAKE_TOOLCHAIN_FILE="$SOURCE_PATH/swift/utils/webassembly/toolchain-wasi.cmake" \
  -DLLVM_BIN="$DESTINATION_TOOLCHAIN/usr/bin" \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_Swift_COMPILER_FORCED=ON \
  -DSWIFT_FOUNDATION_PATH=$DESTINATION_TOOLCHAIN/usr/lib/swift_static/wasi/wasm32 \
  "${SOURCE_PATH}/swift-corelibs-xctest"
  
ninja -v
ninja -v install
