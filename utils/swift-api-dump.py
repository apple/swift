#!/usr/bin/env python3

# This tool dumps imported Swift APIs to help validate changes in the
# projection of (Objective-)C APIs into Swift, which is a function of the
# (Objective-)C APIs, any API notes added on top of those APIs, and the
# Clang importer itself. One can execute it to dump the API of a given
# module within a particular SDK, e.g., UIKit from the iOS SDK as seen in
# Swift 4 compatibility mode:
#
#   /path/to/bin/dir/swift-api-dump.py -swift-version 4 -o output-dir \
#       -m UIKit -s iphoneos
#
# The "-m" argument can be omitted, in which case the script will collect
# all of the frameworks in the named SDK(s) and dump their APIs.
#
# One can supply multiple SDKs, written as a list. For example, to
# dump the API for all frameworks across macOS, iOS, watchOS, and tvOS,
# in Swift 4.2, use:
#
#  /path/to/bin/dir/swift-api-dump.py -swift-version 4.2 -o output-dir \
#      -s macosx iphoneos watchos appletvos
#

import argparse
import multiprocessing
import os
import re
import subprocess
import sys

DEFAULT_TARGET_BASED_ON_SDK = {
    'macosx': 'x86_64-apple-macosx10.11',
    'iphoneos': 'arm64-apple-ios9.0',
    'iphonesimulator': 'x86_64-apple-ios9.0',
    'watchos': 'armv7k-apple-watchos2.0',
    'watchos.simulator': 'i386-apple-watchos2.0',
    'appletvos': 'arm64-apple-tvos9',
    'appletvos.simulator': 'x86_64-apple-tvos9',
}

SKIPPED_FRAMEWORKS = {
    'AppKitScripting',
    'CalendarStore',
    'CoreMIDIServer',
    'DrawSprocket',
    'DVComponentGlue',
    'InstallerPlugins',
    'InstantMessage',
    'JavaFrameEmbedding',
    'JavaVM',
    'Kerberos',
    'Kernel',
    'LDAP',
    'Message',
    'PCSC',
    'PubSub',
    'QTKit',
    'QuickTime',
    'Ruby',
    'Scripting',
    'SyncServices',
    'System',
    'Tk',
    'VideoDecodeAcceleration',
    'vecLib',
}


def create_parser():
    script_path = os.path.dirname(sys.argv[0])
    script_path = os.path.abspath(script_path)
    default_swift_ide_test = '%s/swift-ide-test' % (script_path)

    parser = argparse.ArgumentParser(
        description="Dumps imported Swift APIs for a module or SDK",
        prog='swift-api-dump.py',
        usage='%(prog)s -s iphoneos')
    parser.add_argument('-m', '--module', help='The module name.')
    parser.add_argument('-j', '--jobs', type=int,
                        help='The number of parallel jobs to execute')
    parser.add_argument('-s', '--sdk', nargs='+',
                        required=True, help="The SDKs to use.")
    parser.add_argument('-t', '--target', help="The target triple to use.")
    parser.add_argument('-i', '--swift-ide-test',
                        default=default_swift_ide_test,
                        help="The swift-ide-test executable.")
    parser.add_argument('-o', '--output-dir', default=os.getcwd(),
                        help='Directory to which the output will be emitted.')
    parser.add_argument('-q', '--quiet', action='store_true',
                        help='Suppress printing of status messages.')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Print extra information.')
    parser.add_argument('-F', '--framework-dir', action='append',
                        help='Add additional framework directories')
    parser.add_argument('-iframework', '--system-framework-dir',
                        action='append',
                        help='Add additional system framework directories')
    parser.add_argument('-I', '--include-dir', action='append',
                        help='Add additional include directories')
    parser.add_argument('--enable-infer-import-as-member', action='store_true',
                        help='Infer when a global could be imported as a ' +
                        'member.')
    parser.add_argument('--enable-experimental-concurrency', action='store_true',
                        help='Enable experimental concurrency model.')
    parser.add_argument('--enable-experimental-distributed', action='store_true',
                        help='Enable experimental distributed actors.')
    parser.add_argument('--enable-experimental-observation', action='store_true',
                        help='Enable experimental observation.')
    parser.add_argument('--enable-synchronization', action='store_true',
                        help='Enable Synchronization.')
    parser.add_argument('--enable-volatile', action='store_true',
                        help='Enable Volatile.')
    parser.add_argument('-swift-version', metavar='N',
                        help='the Swift version to use')
    parser.add_argument('-show-overlay', action='store_true',
                        help='Show overlay API in addition to Objective-C ' +
                        'module API')
    parser.add_argument('-show-doc-comments', action='store_true',
                        help='Show documentation comments')
    parser.add_argument('-show-unavailable', action='store_true',
                        help='Show declarations that are unavailable in Swift')
    return parser


def output_command_result_to_file(command_args, filename):
    with open(filename, 'w') as output_file:
        subprocess.call(command_args, stdout=output_file)


def run_command(args):
    proc = subprocess.Popen(
        args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out, err = proc.communicate()
    out = out.decode('UTF8')
    exitcode = proc.returncode
    return (exitcode, out, err)

# Collect the set of submodules for the given module.


def collect_submodules(common_args, module):
    # Execute swift-ide-test to print the interface.
    my_args = ['-module-print-submodules', '-module-to-print=%s' % (module)]
    (exitcode, out, _) = run_command(common_args + my_args)
    if exitcode != 0:
        print(
            'error: submodule collection failed for module %s with error %d' %
            (module, exitcode))
        return ()

    # Find all of the submodule imports.
    import_matcher = re.compile(r'.*import\s+%s\.([A-Za-z_0-9.]+)' % (module))
    submodules = set()
    for line in out.splitlines():
        match = import_matcher.match(line)
        if match:
            submodules.add(match.group(1))

    return sorted(list(submodules))

# Print out the command we're about to execute


def print_command(cmd, outfile=""):
    retstr = " ".join(cmd)
    if outfile != "":
        retstr += " > " + outfile
    print(retstr)

# Dump the API for the given module.


def dump_module_api_star(pack):
    dump_module_api(*pack)


def dump_module_api(cmd, extra_dump_args, output_dir, module, quiet, verbose):
    # Collect the submodules
    submodules = collect_submodules(cmd, module)

    # Dump the top-level module
    if verbose:
        print("mkdir -p %s/%s" % (output_dir, module))
    subprocess.call(['mkdir', '-p', ('%s/%s' % (output_dir, module))])
    output_file = '%s/%s/%s.swift' % (output_dir, module, module)
    if not quiet:
        print('Writing %s...' % output_file)

    top_level_cmd = cmd + extra_dump_args + ['-module-to-print=%s' % (module)]
    if verbose:
        print_command(top_level_cmd, output_file)

    output_command_result_to_file(top_level_cmd, output_file)

    # Dump each submodule.
    for submodule in submodules:
        output_file = '%s/%s/%s.swift' % (output_dir, module, submodule)
        if not quiet:
            print('Writing %s...' % output_file)

        full_submodule = '%s.%s' % (module, submodule)
        submodule_cmd = cmd + extra_dump_args
        submodule_cmd = submodule_cmd + \
            ['-module-to-print=%s' % (full_submodule)]
        if verbose:
            print_command(submodule_cmd, output_file)

        output_command_result_to_file(submodule_cmd, output_file)

    return


def pretty_sdk_name(sdk):
    if sdk.find("macosx") == 0:
        return 'macOS'
    if sdk.find("iphoneos") == 0:
        return 'iOS'
    if sdk.find("watchos") == 0:
        return 'watchOS'
    if sdk.find("appletvos") == 0:
        return 'tvOS'
    return 'unknownOS'

# Collect the set of frameworks we should dump


def collect_frameworks(sdk):
    (exitcode, sdk_path, _) = run_command(
        ["xcrun", "--show-sdk-path", "-sdk", sdk])
    if exitcode != 0:
        print('error: framework collection failed to find SDK path for %s '
              'with error %d' % (sdk, exitcode))
        return ()
    sdk_path = sdk_path.rstrip()

    (exitcode, sdk_version, _) = run_command(
        ["xcrun", "--show-sdk-version", "-sdk", sdk])
    if exitcode != 0:
        print('error: framework collection failed to find SDK version for %s '
              'with error %d' % (sdk, exitcode))
        return ()
    sdk_version = sdk_version.rstrip()

    print('Collecting frameworks from %s %s at %s' %
          (pretty_sdk_name(sdk), sdk_version, sdk_path))

    # Collect all of the framework names
    frameworks_dir = '%s/System/Library/Frameworks' % sdk_path
    framework_matcher = re.compile(r'([A-Za-z_0-9.]+)\.framework')
    frameworks = set()
    for entry in os.listdir(frameworks_dir):
        match = framework_matcher.match(entry)
        if match:
            framework = match.group(1)
            if framework not in SKIPPED_FRAMEWORKS:
                frameworks.add(framework)

    return (sorted(list(frameworks)), sdk_path)


def get_short_sdk_name(sdk):
    matched = re.match("[a-zA-Z]+", sdk)
    return matched.group(0)


def create_dump_module_api_args(cmd_common, cmd_extra_args, sdk, module,
                                target, output_dir, quiet, verbose):

    # Determine the SDK root and collect the set of frameworks.
    (frameworks, sdk_root) = collect_frameworks(sdk)

    # Figure out the "short" name of the SDK
    short_sdk_name = get_short_sdk_name(sdk)

    # Determine the default target.
    if target:
        sdk_target = target
    else:
        sdk_target = DEFAULT_TARGET_BASED_ON_SDK[short_sdk_name]

    # Determine the output idirectory
    pretty_sdk = pretty_sdk_name(short_sdk_name)
    sdk_output_dir = '%s/%s' % (output_dir, pretty_sdk)

    # Create the sets of arguments to dump_module_api.
    results = []
    cmd = cmd_common + ['-sdk', sdk_root, '-target', sdk_target]
    if module:
        results.append(
            (cmd, cmd_extra_args, sdk_output_dir, module, quiet, verbose))
    else:
        for framework in frameworks:
            results.append(
                (cmd, cmd_extra_args, sdk_output_dir, framework, quiet,
                 verbose))

    return results


def main():
    source_filename = 'swift-api-dump.swift'
    parser = create_parser()
    args = parser.parse_args()

    cmd_common = [
        args.swift_ide_test,
        '-print-module',
        '-source-filename',
        source_filename,
        '-skip-overrides'
    ]

    # Add -module-print-skip-overlay
    if not args.show_overlay:
        cmd_common += ['-module-print-skip-overlay']

    # Add -skip-print-doc-comments
    if not args.show_doc_comments:
        cmd_common += ['-skip-print-doc-comments']

    # Add -skip-unavailable
    if not args.show_unavailable:
        cmd_common += ['-skip-unavailable']

    # Add -F / -iframework / -I arguments.
    if args.framework_dir:
        for path in args.framework_dir:
            cmd_common = cmd_common + ['-F', path]
    if args.system_framework_dir:
        for path in args.system_framework_dir:
            cmd_common = cmd_common + ['-iframework', path]
    if args.include_dir:
        for path in args.include_dir:
            cmd_common = cmd_common + ['-I', path]

    # Determine the set of extra arguments we'll use.
    extra_args = ['-skip-imports']
    if args.enable_experimental_concurrency:
        extra_args = extra_args + ['-enable-experimental-concurrency']
    if args.swift_version:
        extra_args = extra_args + ['-swift-version', '%s' % args.swift_version]

    # Create a .swift file we can feed into swift-ide-test
    subprocess.call(['touch', source_filename])

    # Construct the set of API dumps we should perform.
    jobs = []
    for sdk in args.sdk:
        jobs = jobs + create_dump_module_api_args(
            cmd_common, extra_args, sdk, args.module,
            args.target, args.output_dir,
            args.quiet, args.verbose)

    # Execute the API dumps
    pool = multiprocessing.Pool(processes=args.jobs)
    pool.map(dump_module_api_star, jobs)

    # Remove the .swift file we fed into swift-ide-test
    subprocess.call(['rm', '-f', source_filename])


if __name__ == '__main__':
    main()
