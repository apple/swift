#!/bin/bash

set -ex

SOURCE_PATH="$( cd "$(dirname $0)/../../.." && pwd  )" 
SWIFT_PATH=$SOURCE_PATH/swift
UTILS_PATH=$SWIFT_PATH/utils/webassembly

case $(uname -s) in
  Darwin)
    DEPENDENCIES_SCRIPT=$UTILS_PATH/macos/install-dependencies.sh
    HOST_SUFFIX=macosx-$(uname -m)
  ;;
  Linux)
    DEPENDENCIES_SCRIPT=$UTILS_PATH/linux/install-dependencies.sh
    HOST_SUFFIX=linux-$(uname -m)
  ;;
  *)
    echo "Unrecognised platform $(uname -s)"
    exit 1
  ;;
esac

BUILD_SCRIPT=$UTILS_PATH/build-toolchain.sh
RUN_TEST_BIN=$SWIFT_PATH/utils/run-test
TARGET_STDLIB_BUILD_DIR=$SOURCE_PATH/target-build/swift-stdlib-wasi-wasm32

$DEPENDENCIES_SCRIPT
$UTILS_PATH/install-build-sdk.sh

export PATH="$HOME/.wasmer/bin:$PATH"

export SCCACHE_CACHE_SIZE="50G"
export SCCACHE_DIR="$SOURCE_PATH/build-cache"

$BUILD_SCRIPT

echo "Build script completed, will attempt to run test suites..."

# workaround: host target test directory is necessary to use run-test
mkdir -p "$TARGET_STDLIB_BUILD_DIR/test-$HOST_SUFFIX"

# Run tests
$RUN_TEST_BIN --build-dir "$TARGET_STDLIB_BUILD_DIR" --target wasi-wasm32 \
  "$TARGET_STDLIB_BUILD_DIR/test-wasi-wasm32/stdlib"
$RUN_TEST_BIN --build-dir "$TARGET_STDLIB_BUILD_DIR" --target wasi-wasm32 \
  "$TARGET_STDLIB_BUILD_DIR/test-wasi-wasm32/LTO"

if [[ "$(uname)" == "Linux" ]]; then
  echo "Skip running all test suites for Linux"
else
  # Run all tests but ignore failure temporarily
  ninja check-swift-wasi-wasm32 -C "$TARGET_STDLIB_BUILD_DIR" || true
fi

echo "The test suite has finished"
