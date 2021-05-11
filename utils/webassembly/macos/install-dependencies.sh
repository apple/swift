#!/bin/bash

set -ex

pip install six
pip3 install six

brew install cmake ninja llvm sccache

# Install latest wasmer
if [ ! -e ~/.wasmer/bin/wasmer ]; then
  curl https://get.wasmer.io -sSfL | sh
fi

SOURCE_PATH="$(cd "$(dirname $0)/../../../../" && pwd)"
SWIFT_PATH=$SOURCE_PATH/swift
cd $SWIFT_PATH

./utils/update-checkout --clone --scheme wasm --skip-repository swift

cd $SOURCE_PATH

