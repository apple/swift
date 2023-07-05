# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors


import multiprocessing

from build_swift import argparse
from build_swift import defaults

from swift_build_support.swift_build_support import targets


__all__ = [
    'HelpOption',
    'SetOption',
    'SetTrueOption',
    'SetFalseOption',
    'DisableOption',
    'EnableOption',
    'ChoicesOption',
    'IntOption',
    'StrOption',
    'PathOption',
    'AppendOption',
    'UnsupportedOption',
    'IgnoreOption',
    'EXPECTED_OPTIONS',
    'EXPECTED_DEFAULTS',
]


# -----------------------------------------------------------------------------

EXPECTED_DEFAULTS = {
    'android': False,
    'android_api_level': '21',
    'android_deploy_device_path': '/data/local/tmp',
    'android_ndk': None,
    'android_arch': 'armv7',
    'assertions': True,
    'benchmark': False,
    'benchmark_num_o_iterations': 3,
    'benchmark_num_onone_iterations': 3,
    'build_android': False,
    'build_args': [],
    'build_backdeployconcurrency': False,
    'build_benchmarks': True,
    'build_clang_tools_extra': True,
    'build_compiler_rt': True,
    'build_cygwin': True,
    'build_external_benchmarks': False,
    'build_foundation': False,
    'build_cmark': True,
    'build_swift': True,
    'build_llvm': True,
    '_build_llvm': True,
    'build_freebsd': True,
    'build_ios': True,
    'build_ios_device': False,
    'build_ios_simulator': False,
    'build_jobs': multiprocessing.cpu_count(),
    'build_libdispatch': False,
    'build_libicu': False,
    'build_libxml2': False,
    'build_zlib': False,
    'build_curl': False,
    'build_linux': True,
    'build_llbuild': False,
    'build_lldb': False,
    'build_libcxx': False,
    'build_ninja': False,
    'build_lld': False,
    'build_osx': True,
    'build_playgroundsupport': False,
    'build_runtime_with_host_compiler': False,
    'build_stdlib_deployment_targets': ['all'],
    'build_subdir': None,
    'build_swift_dynamic_sdk_overlay': True,
    'build_swift_dynamic_stdlib': True,
    'build_swift_inspect': False,
    'build_swift_private_stdlib': True,
    'build_swift_static_sdk_overlay': False,
    'build_swift_static_stdlib': False,
    'build_swift_stdlib_unittest_extra': False,
    'build_swift_stdlib_static_print': False,
    'build_swift_stdlib_unicode_data': True,
    'build_swift_remote_mirror': True,
    'build_swiftpm': False,
    'build_swift_driver': False,
    'build_swift_libexec': True,
    'build_early_swift_driver': True,
    'build_early_swiftsyntax': True,
    'build_swiftsyntax': False,
    'build_skstresstester': False,
    'build_swiftformat': False,
    'build_swiftevolve': False,
    'build_swiftdocc': False,
    'build_minimalstdlib': False,
    'build_indexstoredb': False,
    'test_indexstoredb_sanitize_all': False,
    'test_sourcekitlsp_sanitize_all': False,
    'build_sourcekitlsp': False,
    'install_llvm': False,
    'install_swiftpm': False,
    'install_swiftsyntax': False,
    'install_swift_driver': False,
    'install_swiftdocc': False,
    'swiftsyntax_verify_generated_files': False,
    'swiftsyntax_enable_rawsyntax_validation': False,
    'swiftsyntax_enable_test_fuzzing': False,
    'swiftsyntax_lint': False,
    'install_playgroundsupport': False,
    'install_sourcekitlsp': False,
    'install_swiftformat': False,
    'install_skstresstester': False,
    'install_swiftevolve': False,
    'build_toolchainbenchmarks': False,
    'build_toolchain_only': False,
    'build_tvos': True,
    'build_tvos_device': False,
    'build_tvos_simulator': False,
    'build_variant': 'Debug',
    'build_watchos': True,
    'build_watchos_device': False,
    'build_watchos_simulator': False,
    'build_xctest': False,
    'cmake_c_launcher': None,
    'cmake_cxx_launcher': None,
    'clang_compiler_version': None,
    'clang_profile_instr_use': None,
    'clang_user_visible_version': defaults.CLANG_USER_VISIBLE_VERSION,
    'clean': False,
    'cmake': None,
    'cmake_generator': 'Ninja',
    'cmark_assertions': True,
    'cmark_build_variant': 'Debug',
    'compiler_vendor': defaults.COMPILER_VENDOR,
    'coverage_db': None,
    'cross_compile_append_host_target_to_destdir': True,
    'cross_compile_deps_path': None,
    'cross_compile_hosts': [],
    'infer_cross_compile_hosts_on_darwin': False,
    'darwin_deployment_version_ios':
        defaults.DARWIN_DEPLOYMENT_VERSION_IOS,
    'darwin_deployment_version_osx':
        defaults.DARWIN_DEPLOYMENT_VERSION_OSX,
    'darwin_deployment_version_tvos':
        defaults.DARWIN_DEPLOYMENT_VERSION_TVOS,
    'darwin_deployment_version_watchos':
        defaults.DARWIN_DEPLOYMENT_VERSION_WATCHOS,
    'darwin_symroot_path_filters': [],
    'darwin_xcrun_toolchain': None,
    'distcc': False,
    'sccache': False,
    'dry_run': False,
    'dsymutil_jobs': defaults.DSYMUTIL_JOBS,
    'enable_asan': False,
    'enable_experimental_differentiable_programming': True,
    'enable_experimental_concurrency': True,
    'enable_experimental_cxx_interop': True,
    'enable_cxx_interop_swift_bridging_header': True,
    'enable_experimental_distributed': True,
    'enable_experimental_string_processing': True,
    'enable_experimental_observation': True,
    'swift_enable_backtracing': True,
    'enable_lsan': False,
    'enable_sanitize_coverage': False,
    'disable_guaranteed_normal_arguments': False,
    'enable_stdlibcore_exclusivity_checking': False,
    'enable_tsan': False,
    'enable_tsan_runtime': False,
    'enable_ubsan': False,
    'export_compile_commands': False,
    'extra_cmake_options': [],
    'extra_swift_args': [],
    'force_optimized_typechecker': False,
    'foundation_build_variant': 'Debug',
    'host_cc': None,
    'host_cxx': None,
    'host_libtool': None,
    'host_lipo': None,
    'host_target': targets.StdlibDeploymentTarget.host_target().name,
    'host_test': False,
    'only_executable_test': False,
    'only_non_executable_test': False,
    'infer_dependencies': False,
    'install_backdeployconcurrency': False,
    'install_prefix': targets.install_prefix(),
    'install_symroot': None,
    'install_destdir': None,
    'install_all': False,
    'ios': False,
    'ios_all': False,
    'legacy_impl': False,
    'libdispatch_build_variant': 'Debug',
    'libicu_build_variant': 'Debug',
    'libxml2_build_variant': 'Debug',
    'lit_jobs': multiprocessing.cpu_count(),
    'zlib_build_variant': 'Debug',
    'curl_build_variant': 'Debug',
    'bootstrapping_mode': None,
    'lit_args': '-sv',
    'llbuild_assertions': True,
    'lldb_assertions': True,
    'lldb_build_variant': 'Debug',
    'lldb_build_with_xcode': '0',
    'llvm_assertions': True,
    'llvm_build_variant': 'Debug',
    'llvm_cmake_options': [],
    'llvm_enable_modules': False,
    'llvm_include_tests': True,
    'llvm_ninja_targets': [],
    'llvm_ninja_targets_for_cross_compile_hosts': [],
    'llvm_max_parallel_lto_link_jobs':
        defaults.LLVM_MAX_PARALLEL_LTO_LINK_JOBS,
    'llvm_targets_to_build': 'X86;ARM;AArch64;PowerPC;SystemZ;Mips;RISCV',
    'tsan_libdispatch_test': False,
    'long_test': False,
    'lto_type': None,
    'maccatalyst': False,
    'maccatalyst_ios_tests': False,
    'native_clang_tools_path': None,
    'native_llvm_tools_path': None,
    'native_swift_tools_path': None,
    'dump_config': False,
    'reconfigure': False,
    'relocate_xdg_cache_home_under_build_subdir': False,
    'show_sdks': False,
    'skip_build': False,
    'skip_local_build': False,
    'stdlib_deployment_targets': None,
    'stress_test': False,
    'swift_analyze_code_coverage': defaults.SWIFT_ANALYZE_CODE_COVERAGE,
    'swift_assertions': True,
    'swift_build_variant': 'Debug',
    'swift_compiler_version': None,
    'swift_disable_dead_stripping': False,
    'swift_darwin_module_archs': None,
    'swift_darwin_supported_archs': None,
    'swift_freestanding_is_darwin': False,
    'swift_profile_instr_use': None,
    'swift_runtime_fixed_backtracer_path': None,
    'swift_stdlib_assertions': True,
    'swift_stdlib_build_variant': 'Debug',
    'swift_tools_ld64_lto_codegen_only_for_supporting_targets': False,
    'swift_tools_max_parallel_lto_link_jobs':
        defaults.SWIFT_MAX_PARALLEL_LTO_LINK_JOBS,
    'swift_user_visible_version': defaults.SWIFT_USER_VISIBLE_VERSION,
    'symbols_package': None,
    'clean_libdispatch': True,
    'clean_foundation': True,
    'clean_xctest': True,
    'clean_llbuild': True,
    'clean_swiftpm': True,
    'clean_swift_driver': True,
    'clean_early_swift_driver': False,
    'test': None,
    'test_early_swift_driver': None,
    'test_android': False,
    'test_android_host': False,
    'test_cygwin': False,
    'test_freebsd': False,
    'test_ios': False,
    'test_watchos_32bit_simulator': False,
    'test_ios_host': False,
    'test_ios_simulator': False,
    'test_linux': False,
    'test_optimize_for_size': None,
    'test_optimize_none_with_implicit_dynamic': None,
    'test_optimized': None,
    'test_osx': False,
    'test_paths': [],
    'test_swift_inspect': True,
    'test_tvos': False,
    'test_tvos_host': False,
    'test_tvos_simulator': False,
    'test_watchos': False,
    'test_watchos_host': False,
    'test_watchos_simulator': False,
    'test_playgroundsupport': True,
    'test_cmark': False,
    'test_swiftpm': False,
    'test_swift_driver': False,
    'test_swiftsyntax': False,
    'test_indexstoredb': False,
    'test_sourcekitlsp': False,
    'test_skstresstester': False,
    'test_swiftformat': False,
    'test_swiftevolve': False,
    'test_swiftdocc': False,
    'test_toolchainbenchmarks': False,
    'tvos': False,
    'tvos_all': False,
    'validation_test': None,
    'verbose_build': False,
    'watchos': False,
    'watchos_all': False,
    'llvm_install_components': defaults.llvm_install_components(),
    'clean_install_destdir': False,
}


# -----------------------------------------------------------------------------

def _sanitize_option_string(option_string):
    if option_string.startswith('--'):
        return option_string[2:].replace('-', '_')

    if len(option_string) == 2 and option_string[0] == '-':
        return option_string[1]

    raise ValueError('invalid option_string format: ' + option_string)


class _BaseOption(object):

    def __init__(self, option_string, dest=None, default=None):
        if dest is None:
            dest = _sanitize_option_string(option_string)

        if default is None:
            default = EXPECTED_DEFAULTS.get(dest, None)

        self.option_string = option_string
        self.dest = dest
        self.default = default

    def sanitized_string(self):
        return _sanitize_option_string(self.option_string)


class HelpOption(_BaseOption):
    """Option that prints the help message and exits."""

    pass


class SetOption(_BaseOption):
    """Option that accepts no arguments, setting the destination to a
    hard-coded value or None.
    """

    def __init__(self, *args, **kwargs):
        self.value = kwargs.pop('value', None)
        super(SetOption, self).__init__(*args, **kwargs)


class SetTrueOption(_BaseOption):
    """Option that accepts no arguments, setting the destination value to True
    if parsed and defaulting to False otherwise.
    """

    pass


class SetFalseOption(_BaseOption):
    """Option that accepts no arguments, setting the destination value to False
    if parsed and defaulting to True otherwise.
    """

    pass


class EnableOption(_BaseOption):
    """Option that sets the destination to True when parsed and False by default.
    Can be toggled True or False with an optional bool argument.
    """

    pass


class DisableOption(_BaseOption):
    """Option that sets the destination to False when parsed and True by default.
    Can be toggled True or False with an optional bool argument, which is then
    negated. Thus if an option is passed the value 'True' it will set the
    destination to False and vice versa.
    """

    pass


class ChoicesOption(_BaseOption):
    """Option that accepts an argument from a predefined list of choices."""

    def __init__(self, *args, **kwargs):
        self.choices = kwargs.pop('choices', None)
        super(ChoicesOption, self).__init__(*args, **kwargs)


class IntOption(_BaseOption):
    """Option that accepts an int argument."""

    pass


class StrOption(_BaseOption):
    """Option that accepts a str argument."""

    pass


class PathOption(_BaseOption):
    """Option that accepts a path argument."""

    pass


class AppendOption(_BaseOption):
    """Option that can be called more than once to append argument to internal
    list.
    """

    pass


class UnsupportedOption(_BaseOption):
    """Option that is not supported."""

    pass


class IgnoreOption(_BaseOption):
    """Option that should be ignored when generating tests. Instead a test
    should be written manually as the behavior cannot or should not be auto-
    generated.
    """

    pass


class BuildScriptImplOption(_BaseOption):
    """Option that gets forwarded to build-script-impl by migration.py and is
    only listed for disambiguation by argparse.
    """

    pass


# -----------------------------------------------------------------------------

EXPECTED_OPTIONS = [
    # Ignore the help options since they always call sys.exit(0)
    HelpOption('-h', dest='help', default=argparse.SUPPRESS),
    HelpOption('--help', dest='help', default=argparse.SUPPRESS),

    SetOption('--debug', dest='build_variant', value='Debug'),
    SetOption('--debug-cmark', dest='cmark_build_variant', value='Debug'),
    SetOption('--debug-foundation',
              dest='foundation_build_variant', value='Debug'),
    SetOption('--debug-libdispatch',
              dest='libdispatch_build_variant', value='Debug'),
    SetOption('--debug-libicu', dest='libicu_build_variant', value='Debug'),
    SetOption('--debug-libxml2', dest='libxml2_build_variant', value='Debug'),
    SetOption('--debug-zlib', dest='zlib_build_variant', value='Debug'),
    SetOption('--debug-curl', dest='curl_build_variant', value='Debug'),
    SetOption('--debug-lldb', dest='lldb_build_variant', value='Debug'),
    SetOption('--lldb-build-with-xcode', dest='lldb_build_with_xcode',
              value='1'),
    SetOption('--lldb-build-with-cmake', dest='lldb_build_with_xcode',
              value='0'),
    SetOption('--debug-llvm', dest='llvm_build_variant', value='Debug'),
    SetOption('--debug-swift', dest='swift_build_variant', value='Debug'),
    SetOption('--debug-swift-stdlib',
              dest='swift_stdlib_build_variant', value='Debug'),
    SetOption('--eclipse',
              dest='cmake_generator', value='Eclipse CDT4 - Ninja'),
    SetOption('--make', dest='cmake_generator', value='Unix Makefiles'),
    SetOption('--release', dest='build_variant', value='Release'),
    SetOption('--release-debuginfo',
              dest='build_variant', value='RelWithDebInfo'),
    SetOption('--min-size-release',
              dest='build_variant', value='MinSizeRel'),
    SetOption('--xcode', dest='cmake_generator', value='Xcode'),
    SetOption('-R', dest='build_variant', value='Release'),
    SetOption('-d', dest='build_variant', value='Debug'),
    SetOption('-e', dest='cmake_generator', value='Eclipse CDT4 - Ninja'),
    SetOption('-m', dest='cmake_generator', value='Unix Makefiles'),
    SetOption('-r', dest='build_variant', value='RelWithDebInfo'),
    SetOption('-x', dest='cmake_generator', value='Xcode'),

    # FIXME: Convert these options to set_true actions
    SetOption('--assertions', value=True),
    SetOption('--cmark-assertions', value=True),
    SetOption('--lldb-assertions', value=True),
    SetOption('--llvm-assertions', value=True),
    SetOption('--llbuild-assertions', value=True),
    SetOption('--swift-assertions', value=True),
    SetOption('--swift-stdlib-assertions', value=True),
    SetOption('-T', dest='validation_test', value=True),
    SetOption('-o', dest='test_optimized', value=True),
    SetOption('-s', dest='test_optimize_for_size', value=True),
    SetOption('-y',
              dest='test_optimize_none_with_implicit_dynamic', value=True),
    SetOption('-t', dest='test', value=True),
    SetOption('-a', dest='assertions', value=True),

    # FIXME: Convert these options to set_false actions
    SetOption('--no-assertions', dest='assertions', value=False),
    SetOption('-A', dest='assertions', value=False),
    SetOption('--no-lldb-assertions', dest='lldb_assertions', value=False),
    SetOption('--no-llvm-assertions', dest='llvm_assertions', value=False),
    SetOption('--no-llbuild-assertions',
              dest='llbuild_assertions', value=False),
    SetOption('--no-swift-assertions', dest='swift_assertions', value=False),
    SetOption('--no-swift-stdlib-assertions',
              dest='swift_stdlib_assertions', value=False),
    SetOption('--skip-ios', dest='ios', value=False),
    SetOption('--skip-tvos', dest='tvos', value=False),
    SetOption('--skip-watchos', dest='watchos', value=False),
    SetOption('--skip-test-early-swift-driver',
              dest='test_early_swift_driver', value=False),

    SetFalseOption('--no-llvm-include-tests', dest='llvm_include_tests'),

    SetTrueOption('--back-deploy-concurrency', dest='build_backdeployconcurrency'),
    SetTrueOption('--install-back-deploy-concurrency',
                  dest='install_backdeployconcurrency'),
    SetTrueOption('--benchmark'),
    SetTrueOption('--clean'),
    SetTrueOption('--clean-install-destdir'),
    SetTrueOption('--dry-run'),
    SetTrueOption('--dump-config'),
    SetTrueOption('--disable-guaranteed-normal-arguments'),
    SetTrueOption('--enable-stdlibcore-exclusivity-checking'),
    SetTrueOption('--force-optimized-typechecker'),
    SetTrueOption('--ios'),
    SetTrueOption('--llbuild', dest='build_llbuild'),
    SetTrueOption('--lldb', dest='build_lldb'),
    SetTrueOption('--libcxx', dest='build_libcxx'),
    SetTrueOption('--maccatalyst', dest='maccatalyst'),
    SetTrueOption('--maccatalyst-ios-tests', dest='maccatalyst_ios_tests'),
    SetTrueOption('--playgroundsupport', dest='build_playgroundsupport'),
    SetTrueOption('--install-playgroundsupport',
                  dest='install_playgroundsupport'),
    SetTrueOption('--skip-build'),
    SetTrueOption('--swiftpm', dest='build_swiftpm'),
    SetTrueOption('--swift-driver', dest='build_swift_driver'),
    SetTrueOption('--swiftsyntax', dest='build_swiftsyntax'),
    SetTrueOption('--skstresstester', dest='build_skstresstester'),
    SetTrueOption('--swiftformat', dest='build_swiftformat'),
    SetTrueOption('--swiftevolve', dest='build_swiftevolve'),
    SetTrueOption('--swiftdocc', dest='build_swiftdocc'),
    SetTrueOption('--build-minimal-stdlib', dest='build_minimalstdlib'),
    SetTrueOption('-B', dest='benchmark'),
    SetTrueOption('-S', dest='skip_build'),
    SetTrueOption('-b', dest='build_llbuild'),
    SetTrueOption('-c', dest='clean'),
    SetTrueOption('-i', dest='ios'),
    SetTrueOption('-l', dest='build_lldb'),
    SetTrueOption('-n', dest='dry_run'),
    SetTrueOption('-p', dest='build_swiftpm'),

    SetTrueOption('--legacy-impl', dest='legacy_impl'),
    SetTrueOption('--infer', dest='infer_dependencies'),
    SetTrueOption('--reconfigure'),

    EnableOption('--android'),
    EnableOption('--build-external-benchmarks'),
    EnableOption('--build-ninja'),
    EnableOption('--build-lld'),
    EnableOption('--build-runtime-with-host-compiler'),
    EnableOption('--build-swift-dynamic-sdk-overlay'),
    EnableOption('--build-swift-dynamic-stdlib'),
    EnableOption('--build-swift-static-sdk-overlay'),
    EnableOption('--build-swift-static-stdlib'),
    EnableOption('--build-swift-stdlib-unittest-extra'),
    EnableOption('--build-swift-stdlib-static-print'),
    EnableOption('--build-toolchain-only'),
    EnableOption('--build-swift-private-stdlib'),
    EnableOption('--build-swift-stdlib-unicode-data'),
    EnableOption('--build-swift-libexec'),
    EnableOption('--build-swift-remote-mirror'),
    EnableOption('--cross-compile-append-host-target-to-destdir'),
    EnableOption('--distcc'),
    EnableOption('--sccache'),
    EnableOption('--enable-asan'),
    EnableOption('--enable-experimental-differentiable-programming'),
    EnableOption('--enable-experimental-concurrency'),
    EnableOption('--enable-experimental-cxx-interop'),
    EnableOption('--enable-cxx-interop-swift-bridging-header'),
    EnableOption('--enable-experimental-distributed'),
    EnableOption('--enable-experimental-string-processing'),
    EnableOption('--enable-experimental-observation'),
    EnableOption('--enable-lsan'),
    EnableOption('--enable-sanitize-coverage'),
    EnableOption('--enable-tsan'),
    EnableOption('--enable-tsan-runtime'),
    EnableOption('--enable-ubsan'),
    EnableOption('--export-compile-commands'),
    EnableOption('--swift-enable-backtracing'),
    EnableOption('--foundation', dest='build_foundation'),
    EnableOption('--host-test'),
    EnableOption('--only-executable-test'),
    EnableOption('--only-non-executable-test'),
    EnableOption('--libdispatch', dest='build_libdispatch'),
    EnableOption('--libicu', dest='build_libicu'),
    EnableOption('--static-libxml2', dest='build_libxml2'),
    EnableOption('--static-zlib', dest='build_zlib'),
    EnableOption('--static-curl', dest='build_curl'),
    EnableOption('--indexstore-db', dest='build_indexstoredb'),
    EnableOption('--test-indexstore-db-sanitize-all',
                 dest='test_indexstoredb_sanitize_all'),
    EnableOption('--sourcekit-lsp', dest='build_sourcekitlsp'),
    EnableOption('--test-sourcekit-lsp-sanitize-all',
                 dest='test_sourcekitlsp_sanitize_all'),
    EnableOption('--install-llvm', dest='install_llvm'),
    EnableOption('--install-swiftsyntax', dest='install_swiftsyntax'),
    EnableOption('--swiftsyntax-verify-generated-files',
                 dest='swiftsyntax_verify_generated_files'),
    EnableOption('--swiftsyntax-enable-rawsyntax-validation',
                 dest='swiftsyntax_enable_rawsyntax_validation'),
    EnableOption('--swiftsyntax-enable-test-fuzzing',
                 dest='swiftsyntax_enable_test_fuzzing'),
    EnableOption('--swiftsyntax-lint',
                 dest='swiftsyntax_lint'),
    EnableOption('--install-swiftpm', dest='install_swiftpm'),
    EnableOption('--install-swift-driver', dest='install_swift_driver'),
    EnableOption('--install-sourcekit-lsp', dest='install_sourcekitlsp'),
    EnableOption('--install-swiftformat', dest='install_swiftformat'),
    EnableOption('--install-skstresstester', dest='install_skstresstester'),
    EnableOption('--install-swiftevolve', dest='install_swiftevolve'),
    EnableOption('--install-swiftdocc', dest='install_swiftdocc'),
    EnableOption('--toolchain-benchmarks', dest='build_toolchainbenchmarks'),
    EnableOption('--swift-inspect', dest='build_swift_inspect'),
    EnableOption('--tsan-libdispatch-test'),
    EnableOption('--long-test'),
    EnableOption('--show-sdks'),
    EnableOption('--skip-local-build'),
    EnableOption('--stress-test'),
    EnableOption('--test'),
    EnableOption('--test-optimize-for-size'),
    EnableOption('--test-optimize-none-with-implicit-dynamic'),
    EnableOption('--test-optimized'),
    EnableOption('--tvos'),
    EnableOption('--validation-test'),
    EnableOption('--verbose-build'),
    EnableOption('--watchos'),
    EnableOption('--xctest', dest='build_xctest'),
    EnableOption('--swift-disable-dead-stripping'),
    EnableOption('--clean-early-swift-driver', dest='clean_early_swift_driver'),
    EnableOption('--llvm-enable-modules'),
    EnableOption('--build-llvm', dest='_build_llvm'),

    DisableOption('--skip-build-cmark', dest='build_cmark'),
    DisableOption('--skip-build-llvm', dest='build_llvm'),
    DisableOption('--skip-build-swift', dest='build_swift'),

    DisableOption('--skip-build-android', dest='build_android'),
    DisableOption('--skip-build-benchmarks', dest='build_benchmarks'),
    DisableOption('--skip-build-cygwin', dest='build_cygwin'),
    DisableOption('--skip-build-freebsd', dest='build_freebsd'),
    DisableOption('--skip-build-ios', dest='build_ios'),
    DisableOption('--skip-build-ios-device', dest='build_ios_device'),
    DisableOption('--skip-build-ios-simulator',
                  dest='build_ios_simulator'),
    DisableOption('--skip-build-linux', dest='build_linux'),
    DisableOption('--skip-build-osx', dest='build_osx'),
    DisableOption('--skip-build-tvos', dest='build_tvos'),
    DisableOption('--skip-build-tvos-device', dest='build_tvos_device'),
    DisableOption('--skip-build-tvos-simulator',
                  dest='build_tvos_simulator'),
    DisableOption('--skip-build-watchos', dest='build_watchos'),
    DisableOption('--skip-build-watchos-device',
                  dest='build_watchos_device'),
    DisableOption('--skip-build-watchos-simulator',
                  dest='build_watchos_simulator'),
    DisableOption('--skip-clean-libdispatch', dest='clean_libdispatch'),
    DisableOption('--skip-clean-foundation', dest='clean_foundation'),
    DisableOption('--skip-clean-xctest', dest='clean_xctest'),
    DisableOption('--skip-clean-llbuild', dest='clean_llbuild'),
    DisableOption('--skip-early-swift-driver', dest='build_early_swift_driver'),
    DisableOption('--skip-early-swiftsyntax', dest='build_early_swiftsyntax'),
    DisableOption('--skip-clean-swiftpm', dest='clean_swiftpm'),
    DisableOption('--skip-clean-swift-driver', dest='clean_swift_driver'),
    DisableOption('--skip-test-android', dest='test_android'),
    DisableOption('--skip-test-android-host', dest='test_android_host'),
    DisableOption('--skip-test-cygwin', dest='test_cygwin'),
    DisableOption('--skip-test-freebsd', dest='test_freebsd'),
    DisableOption('--skip-test-ios', dest='test_ios'),
    DisableOption('--skip-test-watchos-32bit-simulator',
                  dest='test_watchos_32bit_simulator'),
    DisableOption('--skip-test-ios-host', dest='test_ios_host'),
    DisableOption('--skip-test-ios-simulator', dest='test_ios_simulator'),
    DisableOption('--skip-test-linux', dest='test_linux'),
    DisableOption('--skip-test-osx', dest='test_osx'),
    DisableOption('--skip-test-tvos', dest='test_tvos'),
    DisableOption('--skip-test-tvos-host', dest='test_tvos_host'),
    DisableOption('--skip-test-tvos-simulator',
                  dest='test_tvos_simulator'),
    DisableOption('--skip-test-watchos', dest='test_watchos'),
    DisableOption('--skip-test-watchos-host', dest='test_watchos_host'),
    DisableOption('--skip-test-watchos-simulator',
                  dest='test_watchos_simulator'),
    DisableOption('--skip-test-playgroundsupport',
                  dest='test_playgroundsupport'),
    DisableOption('--skip-test-cmark', dest='test_cmark'),
    DisableOption('--skip-test-swiftpm', dest='test_swiftpm'),
    DisableOption('--skip-test-swift-driver', dest='test_swift_driver'),
    DisableOption('--skip-test-swiftsyntax', dest='test_swiftsyntax'),
    DisableOption('--skip-test-indexstore-db', dest='test_indexstoredb'),
    DisableOption('--skip-test-sourcekit-lsp', dest='test_sourcekitlsp'),
    DisableOption('--skip-test-skstresstester', dest='test_skstresstester'),
    DisableOption('--skip-test-swiftformat', dest='test_swiftformat'),
    DisableOption('--skip-test-swiftevolve', dest='test_swiftevolve'),
    DisableOption('--skip-test-swiftdocc', dest='test_swiftdocc'),
    DisableOption('--skip-test-toolchain-benchmarks',
                  dest='test_toolchainbenchmarks'),
    DisableOption('--skip-test-swift-inspect',
                  dest='test_swift_inspect'),
    DisableOption('--skip-build-clang-tools-extra',
                  dest='build_clang_tools_extra'),
    DisableOption('--skip-build-libxml2', dest='build_libxml2'),
    DisableOption('--skip-build-zlib', dest='build_zlib'),
    DisableOption('--skip-build-curl', dest='build_curl'),
    DisableOption('--skip-build-compiler-rt', dest='build_compiler_rt'),

    ChoicesOption('--compiler-vendor',
                  choices=['none', 'apple']),
    ChoicesOption('--swift-analyze-code-coverage',
                  choices=['false', 'not-merged', 'merged']),
    ChoicesOption('--android-arch',
                  choices=['armv7', 'aarch64']),

    StrOption('--android-api-level'),
    StrOption('--build-args'),
    StrOption('--build-stdlib-deployment-targets'),
    StrOption('--darwin-deployment-version-ios'),
    StrOption('--darwin-deployment-version-osx'),
    StrOption('--darwin-deployment-version-tvos'),
    StrOption('--darwin-deployment-version-watchos'),
    StrOption('--darwin-xcrun-toolchain'),
    StrOption('--host-target'),
    StrOption('--lit-args'),
    StrOption('--llvm-targets-to-build'),
    StrOption('--stdlib-deployment-targets'),
    StrOption('--swift-darwin-module-archs'),
    StrOption('--swift-darwin-supported-archs'),
    SetTrueOption('--swift-freestanding-is-darwin'),

    PathOption('--android-deploy-device-path'),
    PathOption('--android-ndk'),
    PathOption('--build-subdir'),
    SetTrueOption('--relocate-xdg-cache-home-under-build-subdir'),
    PathOption('--clang-profile-instr-use'),
    PathOption('--cmake'),
    PathOption('--coverage-db'),
    PathOption('--cross-compile-deps-path'),
    PathOption('--host-cc'),
    PathOption('--host-cxx'),
    PathOption('--host-libtool'),
    PathOption('--host-lipo'),
    PathOption('--install-prefix'),
    PathOption('--install-symroot'),
    PathOption('--install-destdir'),
    EnableOption('--install-all'),
    PathOption('--native-clang-tools-path'),
    PathOption('--native-llvm-tools-path'),
    PathOption('--native-swift-tools-path'),
    PathOption('--symbols-package'),
    PathOption('--cmake-c-launcher'),
    PathOption('--cmake-cxx-launcher'),
    PathOption('--swift-profile-instr-use'),
    PathOption('--swift-runtime-fixed-backtracer-path'),

    IntOption('--benchmark-num-o-iterations'),
    IntOption('--benchmark-num-onone-iterations'),
    IntOption('--jobs', dest='build_jobs'),
    IntOption('--llvm-max-parallel-lto-link-jobs'),
    IntOption('--swift-tools-max-parallel-lto-link-jobs'),
    EnableOption('--swift-tools-ld64-lto-codegen-only-for-supporting-targets'),
    IntOption('-j', dest='build_jobs'),
    IntOption('--lit-jobs', dest='lit_jobs'),
    IntOption('--dsymutil-jobs', dest='dsymutil_jobs'),

    AppendOption('--cross-compile-hosts'),
    SetTrueOption('--infer-cross-compile-hosts-on-darwin'),
    AppendOption('--extra-cmake-options'),
    AppendOption('--extra-swift-args'),
    AppendOption('--test-paths'),
    AppendOption('--llvm-ninja-targets'),
    AppendOption('--llvm-ninja-targets-for-cross-compile-hosts'),
    AppendOption('--llvm-cmake-options'),
    AppendOption('--darwin-symroot-path-filters'),

    UnsupportedOption('--build-jobs'),
    UnsupportedOption('--common-cmake-options'),
    UnsupportedOption('--only-execute'),
    UnsupportedOption('--skip-test-optimize-for-size'),
    UnsupportedOption('--skip-test-optimize-none-with-implicit-dynamic'),
    UnsupportedOption('--skip-test-optimized'),

    # Options forwarded to build-script-impl
    BuildScriptImplOption('--skip-test-swift', dest='impl_skip_test_swift'),
    BuildScriptImplOption('--install-swift', dest='impl_install_swift'),

    # NOTE: LTO flag is a special case that acts both as an option and has
    # valid choices
    SetOption('--lto', dest='lto_type'),
    ChoicesOption('--lto', dest='lto_type', choices=['thin', 'full']),

    SetOption('--bootstrapping', dest='bootstrapping_mode'),
    ChoicesOption('--bootstrapping', dest='bootstrapping_mode',
                  choices=['off', 'hosttools', 'bootstrapping',
                           'bootstrapping-with-hostlibs']),

    # NOTE: We'll need to manually test the behavior of these since they
    # validate compiler version strings.
    IgnoreOption('--clang-compiler-version'),
    IgnoreOption('--clang-user-visible-version'),
    IgnoreOption('--swift-compiler-version'),
    IgnoreOption('--swift-user-visible-version'),

    # TODO: Migrate to unavailable options once new parser is in place
    IgnoreOption('-I'),
    IgnoreOption('--ios-all'),
    IgnoreOption('--tvos-all'),
    IgnoreOption('--watchos-all'),

    StrOption('--llvm-install-components'),
]
