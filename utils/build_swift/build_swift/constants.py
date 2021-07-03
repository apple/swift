# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors


from __future__ import absolute_import
from __future__ import print_function
from __future__ import unicode_literals

import os.path


__all__ = [
    "BUILD_SCRIPT_IMPL_PATH",
    "BUILD_SCRIPT_PATH",
    "BUILD_SWIFT_PATH",
    "MODULE_PATH",
    "MULTIROOT_DATA_FILE_PATH",
    "PROJECT_PATH",
    "RESOURCES_PATH",
    "SWIFT_BUILD_ROOT",
    "SWIFT_REPO_NAME",
    "SWIFT_SOURCE_ROOT",
    "UTILS_PATH",
]


# --------------------------------------------------------------------------------------
# Project Paths


MODULE_PATH = os.path.abspath(os.path.dirname(__file__))

BUILD_SWIFT_PATH = os.path.dirname(MODULE_PATH)

UTILS_PATH = os.path.dirname(BUILD_SWIFT_PATH)

PROJECT_PATH = os.path.dirname(UTILS_PATH)


BUILD_SCRIPT_PATH = os.path.join(UTILS_PATH, "build-script")

BUILD_SCRIPT_IMPL_PATH = os.path.join(UTILS_PATH, "build-script-impl")


# --------------------------------------------------------------------------------------
# Resources


RESOURCES_PATH = os.path.join(BUILD_SWIFT_PATH, "resources")


# The path to the Xcode workspace to use for a unified build of multiple SwiftPM
# projects.
MULTIROOT_DATA_FILE_PATH = os.path.join(
    RESOURCES_PATH, "SwiftPM-Unified-Build.xcworkspace"
)


# --------------------------------------------------------------------------------------
# Helpers


def _is_llvm_checkout(llvm_path):
    """Returns true if the given llvm_path is a valid LLVM checkout, false otherwise.

    NOTE: This is a very naive validation, checking only for the existence of a few
    known files.
    """

    if not os.path.exists(os.path.join(llvm_path, "tools")):
        return False

    if not os.path.exists(os.path.join(llvm_path, "CMakeLists.txt")):
        return False

    return True


def _is_swift_checkout(swift_path):
    """Returns true if the given swift_path is a valid Swift checkout, false otherwise.

    NOTE: This is a very naive validation, checking only for the existence of a few
    known files.
    """

    if not os.path.exists(os.path.join(swift_path, "utils")):
        return False

    if not os.path.exists(os.path.join(swift_path, "CMakeLists.txt")):
        return False

    return True


def _get_swift_source_root(swift_path, env=None):
    """Returns the Swift source root or None if one cannot be determined.

    Users are able to manually override the source root by setting the SWIFT_SOURCE_ROOT
    environment variable. If that cannot be found then this function will check the
    directory structure to infer if we are building as a standalone Swift build or if we
    are building in the unified LLVM.

    Building standalone means Swift will be checked out as a peer of LLVM and the
    enclosing directory is the source root.

        source-root/
        |- llvm/
        |- swift/
        | ...

    However the unified case means Swift will be checked out in the llvm/tools
    directory, which means the directory containing LLVM is the source root.

        source-root/
        |- llvm/
        |   |- tools/
        |   |   |- swift/
        |   |   | ...
        |   | ...
        | ...

    In the case that this function is called with an invalid Swift checkout it returns
    None as well.

    FIXME: What about the new llvm-project monorepo?
    """

    env = env or {}

    # Check the environment first.
    if "SWIFT_SOURCE_ROOT" in env:
        return env["SWIFT_SOURCE_ROOT"]

    # Assert we are in a valid Swift checkout.
    if not _is_swift_checkout(swift_path):
        return None

    source_root = os.path.dirname(swift_path)

    # Check if Swift is checked out as part of a unified build.
    if os.path.basename(source_root) != "tools":
        return source_root

    llvm_path = os.path.dirname(source_root)
    if not _is_llvm_checkout(llvm_path):
        return source_root

    # Return the directory containing LLVM.
    return os.path.dirname(llvm_path)


def _get_swift_build_root(source_root, env=None):
    """Returns the Swift build root.

    Users are able to manually override the build root by setting the SWIFT_BUILD_ROOT
    environment variable. If that cannot be found then this function returns the path
    to a directory named "build" in the given source root.
    """

    env = env or {}

    if "SWIFT_BUILD_ROOT" in env:
        build_root = env["SWIFT_BUILD_ROOT"]
        if os.path.isabs(build_root):
            return build_root
        return os.path.join(source_root, build_root)

    return os.path.join(source_root, "build")


def _get_swift_repo_name(swift_path, env=None):
    """Returns the Swift repo name or None if it cannot be determined.

    Users are able to manually override the repo name by setting the SWIFT_REPO_NAME
    environment variable. If that cannot be found then this function returns the name
    of the given swift path or None if it is not a valid Swift checkout.
    """

    env = env or {}

    if "SWIFT_REPO_NAME" in env:
        return env["SWIFT_REPO_NAME"]

    if not _is_swift_checkout(swift_path):
        return None

    return os.path.basename(swift_path)


# --------------------------------------------------------------------------------------
# Swift Source and Build Roots


# Set SWIFT_SOURCE_ROOT in your environment to control where the sources are found.
SWIFT_SOURCE_ROOT = _get_swift_source_root(PROJECT_PATH, env=os.environ)

# Set SWIFT_BUILD_ROOT to a directory that will contain a subdirectory for each build
# configuration
SWIFT_BUILD_ROOT = _get_swift_build_root(SWIFT_SOURCE_ROOT, env=os.environ)

# Set SWIFT_REPO_NAME in your environment to control the name of the swift directory
# name that is used.
SWIFT_REPO_NAME = _get_swift_repo_name(PROJECT_PATH, env=os.environ)
