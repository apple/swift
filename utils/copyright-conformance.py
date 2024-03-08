#!/usr/bin/env python3
# This test validates that all source files have correct copyright headers.
# Ignores any files given that don't have a copyright notice
# Only changes copyright headers for files whose years don't conform
# to when they were last modified according to git modification date

import sys
import re
import datetime
import shlex
import subprocess
import argparse

import os.path
from distutils.util import strtobool
from concurrent.futures import ThreadPoolExecutor
from multiprocessing import cpu_count

# Regex to match different types of Copright headers
# matches years from 1000-2999
COPYRIGHT_EXISTS_PATTERN = r'Copyright.*\d{4}'
COPYRIGHT_SINGLE_YEAR_PATTERN = r'[12][0-9]{3}'
COPYRIGHT_RANGE_PATTERN = r'[12][0-9]{3} - [12][0-9]{3}'
COPYRIGHT_LITERAL_PATTERN = r'Copyright \(c\) '

DEBUG = False
VERBOSE = False


def get_parser():
    parser = argparse.ArgumentParser(description='Validates that all source files given on command line \
                                            have correct copyright headers, based on git modification dates. \
                                            Note that this edits the modification date.')
    required = parser.add_argument_group('required arguments')
    required.add_argument('-f', nargs='+', help='One or more files/directories to check', required=True)
    parser.add_argument('-r', '--recursive', help='Recursively search through directories', action="store_true", default=False, required=False)
    parser.add_argument('-d', '--debug', help="Print files whose copyright headers could not be found, or don't exist", action='store_true', default=False, required=False)
    parser.add_argument('-v', '--verbose', help="Print additional info on when copyright headers are conformant, or when files are patched", action='store_true', default=False, required=False)
    parser.add_argument('-p', '--patch', help='Replace non-conforming copyright headers with the correct years', action="store_true", default=False, required=False)
    parser.add_argument('-t', '--threads', help='Number of threads to use when testing files for copyright history', default=cpu_count(), required=False)
    parser.add_argument('-i', '--interactive', help='Shows copyright notice before and after change. Asks for user confirmation when patching files', action="store_true", default=False, required=False)
    return parser


def debug(message):
    global DEBUG
    if DEBUG:
        print(message)


def verbose(message):
    global VERBOSE
    if VERBOSE:
        print(message)


def discover_files(filepaths, recursive_opt):
    """Make sure all files given exist, and discover any files in folders if we're discovering files recursively."""
    # TODO: Make paths relative to swift source directory
    discovered_files = set()

    i = 0
    while i < len(filepaths):
        f = os.path.abspath(filepaths[i])
        # exit if files given on command line don't exist

        if os.path.islink(f):
            debug('Skipping symlink {}'.format(f))
            i += 1
            continue

        if not os.path.exists(f):
            print(f"Unexpected error: No such file or directory {f}", file=sys.stderr)
            sys.exit(1)

        if os.path.isdir(f):
            if recursive_opt:
                dir_contents = [os.path.join(f, path) for path in os.listdir(f)]
                filepaths.extend(dir_contents)
        else:
            discovered_files.add(f)

        i += 1
    return list(discovered_files)


def has_copyright_notice(candidate_filepath):
    """Checks if a file has a copyright notice"""
    with open(candidate_filepath, "r") as f:
        try:
            contents = f.read()
        except UnicodeDecodeError:
            debug("Skipping file {} because it is not UTF-8 encoded".format(candidate_filepath))
            return False
    matches = re.findall(COPYRIGHT_EXISTS_PATTERN, contents)

    has_match = len(matches) > 0
    if has_match:
        verbose(f"Found copyright notice for {candidate_filepath}: {matches[0]}")
    else:
        debug(f"Could not find copyright notice for {candidate_filepath}")

    return has_match


def get_expected_copyright_notice(filepath):
    """Returns the expected copyright notice for a file based on the git commit history."""
    # get the year this file was created/last modified
    out = subprocess.check_output(shlex.split(f'git log --format="%ad" --date="format:%Y" -- {filepath}')).decode("utf-8")
    years = out.splitlines()
    # if this a new file, and has no git commit history, the copyright notice should have the current year
    if len(years) == 0:
        current_year = datetime.datetime.now().year
        expected_created_year, expected_last_modified_year = current_year, current_year
    # if git log returned information on when this file was created/last modified
    else:
        expected_created_year = years[-1]
        expected_last_modified_year = years[0]

    if expected_created_year == expected_last_modified_year:
        return "Copyright (c) {}".format(expected_created_year)
    else:
        return "Copyright (c) {} - {}".format(expected_created_year, expected_last_modified_year)


def validate_copyright_notice(filepath, copyright_notice, expected_copyright_notice):
    """Checks whether copyright notice is conformant, tells user if any copyright headers should be changed."""
    if copyright_notice == expected_copyright_notice:
        verbose("Copyright notice for {} is conformant.".format(filepath))
        return (None, None)
    else:
        print("Non-Conforming copyright notice for {}. Copyright notice is '{}', should be '{}'.".format(
             filepath, copyright_notice, expected_copyright_notice
             ))
        return (filepath, expected_copyright_notice)


def copyright_notice_is_conformant(filepath):
    """Checks the dates in the copyright notice against the git created and last modified dates.
       If copyright notice is non-conforming, returns what it should be changed to.
       Returns None if the copyright notice has the correct years and nothing has to be changed."""

    expected_copyright_notice = get_expected_copyright_notice(filepath)

    # get file contents
    with open(filepath, "r") as f:
        contents = f.read()

    # test contents against regex matching a range of years, a single year, and a final regex for any other spacing issues.
    range_match = re.search(f"{COPYRIGHT_LITERAL_PATTERN}{COPYRIGHT_RANGE_PATTERN}", contents)
    single_match = re.search(f"{COPYRIGHT_LITERAL_PATTERN}{COPYRIGHT_SINGLE_YEAR_PATTERN}", contents)
    exists_match = re.search(f"{COPYRIGHT_EXISTS_PATTERN}", contents)

    if range_match:
        return validate_copyright_notice(filepath, range_match.group(0), expected_copyright_notice)
    elif single_match:
        return validate_copyright_notice(filepath, single_match.group(0), expected_copyright_notice)
    else:
        return validate_copyright_notice(filepath, exists_match.group(0), expected_copyright_notice)


def get_user_confirmation():
    """Asks user for confirmation till they give a valid response."""
    while True:
        try:
            user_input = input('> ')
            return strtobool(user_input)
        except ValueError as v:
            print("Could not interpret {} as a response.".format(user_input))
            print("True values are 'y', 'yes', 't', 'true', 'on', and '1'; false values are 'n', 'no', 'f', 'false', 'off', and '0'")


def find_copyright_line(lines):
    """Returns the line number and match object for the first line which matches the Copyright Notice"""
    for number, line in enumerate(lines):
        match = re.search(COPYRIGHT_EXISTS_PATTERN, line)
        if match:
            return number, match


def patch(non_conforming_filepath, correct_copyright_notice, interactive_opt):
    """Patches the non conforming copyright notice - overwrites that line in the file"""

    # get file contents
    with open(non_conforming_filepath, "r") as f:
        lines = f.readlines()

    write_to_file = True
    line_number, match = find_copyright_line(lines)
    conforming_line = lines[line_number].replace(match.group(0), correct_copyright_notice, 1)

    # if in interactive mode, ask user for confirmation
    if interactive_opt:
        print("Line:\n{}Conforming Line:\n{}Replace this line in the file?".format(
              lines[line_number], conforming_line
              ))
        write_to_file = get_user_confirmation()
    lines[line_number] = conforming_line

    if write_to_file:
        verbose("Patching file {} with correct copyright notice.".format(non_conforming_filepath))
        with open(non_conforming_filepath, "w") as f:
            f.writelines(lines)


def main():
    global DEBUG
    global VERBOSE
    parser = get_parser()
    args = parser.parse_args()
    DEBUG = args.debug
    VERBOSE = args.verbose
    num_threads = int(args.threads)
    if VERBOSE:
        DEBUG = True
    if args.patch:
        # if there are modified files tracked by git, shouldn't patch anything.
        # git status --porcelain will return a list of files that are untracked or have been modified
        out = subprocess.check_output(shlex.split("git status --porcelain")).decode("utf-8")
        # if there was output, can't call patch
        if out.strip():
            print("There are modified files in the git directory - Cannot patch files. Commit any changes and try again.", file=sys.stderr)
            sys.exit(2)
    # discover any files in subdirectories if we're searching recursively
    files = discover_files(args.f, args.recursive)
    # filter filepaths - remove any paths which don't have a copyright notice
    copyright_notice_files = [f for f in files if has_copyright_notice(f)]
    debug('testing copyright notice files with {} threads'.format(num_threads))
    with ThreadPoolExecutor(max_workers=num_threads) as executor:
        for (f, correct_copyright_notice) in executor.map(copyright_notice_is_conformant, copyright_notice_files):
            # if the copyright notice should be changed
            if correct_copyright_notice is not None:
                # if we should be replacing headers in files
                if args.patch:
                    patch(f, correct_copyright_notice, args.interactive)

if __name__ == "__main__":
    main()
