#!/bin/bash

set -eux

SOURCE_PATH="$( cd "$(dirname "$0")/../../../" && pwd  )"
BUILD_SDK_PATH="$SOURCE_PATH/build-sdk"

install_libxml2() {
  LIBXML2_URL="https://github.com/swiftwasm/libxml2-wasm/releases/download/1.0.0/libxml2-wasm32-unknown-wasi.tar.gz"
  curl -L "$LIBXML2_URL" | tar xz
  rm -rf "$BUILD_SDK_PATH/libxml2"
  mv libxml2-wasm32-unknown-wasi "$BUILD_SDK_PATH/libxml2"
}

install_icu() {
  ICU_URL="https://github.com/swiftwasm/icu4c-wasi/releases/download/0.5.0/icu4c-wasi.tar.xz"
  curl -L "$ICU_URL" | tar Jx
  rm -rf "$BUILD_SDK_PATH/icu"
  mv icu_out "$BUILD_SDK_PATH/icu"
}

install_wasi-sdk() {
  # We only use wasi-sysroot and do not use binaries in wasi-sdk,
  # so build machine's os and wasi-sdk's host os don't have to be matched
  WASI_SDK_URL="https://github.com/swiftwasm/wasi-sdk/releases/download/0.2.2-swiftwasm/dist-ubuntu-18.04.zip"

  curl -L -o dist-wasi-sdk.zip "$WASI_SDK_URL"
  unzip -u dist-wasi-sdk.zip -d .

  WASI_SDK_TAR_PATH=$(find . -type f -name "wasi-sdk-*")
  WASI_SDK_FULL_NAME=$(basename "$WASI_SDK_TAR_PATH" -linux.tar.gz)
  tar xfz "$WASI_SDK_TAR_PATH"

  rm -rf "$BUILD_SDK_PATH/wasi-sdk"
  mv "$WASI_SDK_FULL_NAME" "$BUILD_SDK_PATH/wasi-sdk"
}

workdir=$(mktemp -d)
pushd "$workdir"

mkdir -p "$BUILD_SDK_PATH"

if [ ! -e "$BUILD_SDK_PATH/libxml2" ]; then
  install_libxml2
fi

if [ ! -e "$BUILD_SDK_PATH/icu" ]; then
  install_icu
fi

if [ ! -e "$BUILD_SDK_PATH/wasi-sdk" ]; then
  install_wasi-sdk
fi
