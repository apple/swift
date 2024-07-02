#!/usr/bin/env bash
#
# utils/build-toolchain - documents process for building a toolchain
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors

function usage() {
  echo "$0 <bundle_prefix> [OPTIONS]"
  echo ""
  echo "<bundle_prefix> - Prefix to use for bundle name"
  echo ""
  echo "OPTIONS"
  echo ""
  echo "-h --help"
  echo "Show help information."
  echo ""
  echo "-n --dry-run"
  echo "Do a dry run."
  echo ""
  echo "-t --test"
  echo "Run tests."
  echo ""
  echo "-a --no-assert"
  echo "No assertions"
  echo ""
  echo "--distcc"
  echo "Build with distcc to speed up the toolchain build"
  echo ""
  echo "--sccache"
  echo "Build with sccache to speed up the toolchain build"
  echo ""
  echo "--preset-file"
  echo "load build-script presets from the specified file"
  echo ""
  echo "--preset-prefix"
  echo "Customize the preset invoked by prepending a prefix"
  echo ""
  echo "--macos-only"
  echo "On Darwin, only build for macOS instead of for all platforms"
  echo ""
  echo "--use-os-runtime"
  echo "Require this toolchain to link against the OS runtime rather than the toolchains packaged runtime"
  echo ""
}

RESULT_DIR=$PWD
cd "$(dirname $0)/.." || exit

# Set defaults
DISTCC_FLAG=
SCCACHE_FLAG=
DRY_RUN=
BUNDLE_PREFIX=
PRESET_FILE_FLAGS=
PRESET_PREFIX=
NO_TEST=",no_test"
USE_OS_RUNTIME=
MACOS_ONLY=
NO_ASSERTIONS=

case $(uname -s) in
  Darwin)
    SWIFT_PACKAGE=buildbot_osx_package
    OS_SUFFIX=osx
  ;;
  Linux)
    SWIFT_PACKAGE=buildbot_linux
    OS_SUFFIX=linux
  ;;
  *)
    echo "Unrecognised platform $(uname -s)"
    exit 1
  ;;
esac

# Process command line arguments
FIRST_ARG_PROCESSED=0
while [ $# -ne 0 ]; do
  case "$1" in
    -n|--dry-run)
      DRY_RUN="-n"
  ;;
    -t|--test)
      NO_TEST=
  ;;
    --distcc)
      DISTCC_FLAG="--distcc"
  ;;
    --sccache)
      SCCACHE_FLAG="--sccache"
  ;;
    --preset-file)
      shift
      PRESET_FILE_FLAGS="${PRESET_FILE_FLAGS} --preset-file=$1"
  ;;
    --preset-prefix)
      shift
      PRESET_PREFIX="$1"
  ;;
    --use-os-runtime)
      USE_OS_RUNTIME=",use_os_runtime"
  ;;
    --macos-only)
      MACOS_ONLY=",macos_only"
  ;;
    -a|--no-assert)
      NO_ASSERTIONS=",no_assertions"
  ;;
  -h|--help)
    usage
    exit 0
  ;;
  *)
    if [ ${FIRST_ARG_PROCESSED} -eq 0 ]; then
      # This is the bundle prefix
      BUNDLE_PREFIX="$1"
    else
      echo "Unrecognised argument \"$1\""
      exit 1
    fi
  ;;
  esac
  FIRST_ARG_PROCESSED=1
  shift
done

if [ -z "${BUNDLE_PREFIX}" ]; then
  echo "Bundle prefix cannot be empty. See $0 --help"
  exit 1
fi

# Report the commands being run
set -x
YEAR=$(date +"%Y")
MONTH=$(date +"%m")
DAY=$(date +"%d")
TOOLCHAIN_VERSION="5.8.${YEAR}${MONTH}${DAY}"
TOOLCHAIN_NAME="swift-LOCAL-${YEAR}-${MONTH}-${DAY}-a"
DARWIN_TOOLCHAIN_VERSION="0.0.${YEAR}${MONTH}${DAY}"
ARCHIVE="${TOOLCHAIN_NAME}-${OS_SUFFIX}.tar.gz"
SYM_ARCHIVE="${TOOLCHAIN_NAME}-${OS_SUFFIX}-symbols.tar.gz"
BUNDLE_PREFIX=${BUNDLE_PREFIX:?Please specify a bundle prefix}
BUNDLE_IDENTIFIER="${BUNDLE_PREFIX}.${YEAR}${MONTH}${DAY}"
DISPLAY_NAME_SHORT="Local Swift Development Snapshot"
DISPLAY_NAME="${DISPLAY_NAME_SHORT} ${YEAR}-${MONTH}-${DAY}"

SWIFT_INSTALLABLE_PACKAGE="${RESULT_DIR}/${ARCHIVE}"
SWIFT_INSTALL_DIR="${RESULT_DIR}/swift-nightly-install"
SWIFT_INSTALL_SYMROOT="${RESULT_DIR}/swift-nightly-symroot"
SWIFT_TOOLCHAIN_DIR="/Library/Developer/Toolchains/${TOOLCHAIN_NAME}.xctoolchain"
SYMBOLS_PACKAGE="${RESULT_DIR}/${SYM_ARCHIVE}"
DRY_RUN="${DRY_RUN}"
DISTCC_FLAG="${DISTCC_FLAG}"
PRESET_FILE_FLAGS="${PRESET_FILE_FLAGS}"
SCCACHE_FLAG="${SCCACHE_FLAG}"

./utils/build-script ${DRY_RUN} ${DISTCC_FLAG} ${PRESET_FILE_FLAGS} \
        ${SCCACHE_FLAG} \
        --preset="${PRESET_PREFIX}${SWIFT_PACKAGE}${NO_ASSERTIONS}${NO_TEST}${USE_OS_RUNTIME}${MACOS_ONLY}" \
        install_destdir="${SWIFT_INSTALL_DIR}" \
        installable_package="${SWIFT_INSTALLABLE_PACKAGE}" \
        install_toolchain_dir="${SWIFT_TOOLCHAIN_DIR}" \
        install_symroot="${SWIFT_INSTALL_SYMROOT}" \
        symbols_package="${SYMBOLS_PACKAGE}" \
        darwin_toolchain_bundle_identifier="${BUNDLE_IDENTIFIER}" \
        darwin_toolchain_display_name="${DISPLAY_NAME}" \
        darwin_toolchain_display_name_short="${DISPLAY_NAME_SHORT}" \
        darwin_toolchain_xctoolchain_name="${TOOLCHAIN_NAME}" \
        darwin_toolchain_version="${TOOLCHAIN_VERSION}" \
        darwin_toolchain_alias="Local" \
        darwin_toolchain_require_use_os_runtime="${REQUIRE_USE_OS_RUNTIME}"
