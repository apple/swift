# ====--- compare.py - Compare built products' sizes -*- coding: utf-8 -*-===//
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors

import collections
import os
import re
import subprocess
from operator import itemgetter

categories = [
    # Cpp
    ["CPP", re.compile('^(__Z|_+swift)')],

    # Objective-C
    ["ObjC", re.compile(r'^[+-]\[')],

    # Swift
    ["Partial Apply", re.compile('^__(TPA|T0.*T[aA]$)')],
    ["Protocol Witness", re.compile('^__(TTW|T0.*TW$)')],
    ["Value Witness", re.compile('^__(Tw|T0.*w..$)')],
    ["Type Metadata", re.compile('^__(TM|T0.*(N|M.)$)')],
    # Function signature specialization of a generic specialization.
    ["FuncSigGen Spec", re.compile(
        '^__(TTSf.*__TTSg|T0.*T[gGpP]q?[0-9].*Tfq?[0-9])')],
    ["Generic Spec", re.compile('^__(TTSg|T0.*T[gG]q?[0-9])')],
    ["Partial Spec", re.compile('^__(T0.*T[pP]q?[0-9])')],
    ["FuncSig Spec", re.compile('^__(TTSf|T0.*Tfq?[0-9])')],
    ["Generic Function", re.compile(
        '__(T[^0].*q(x|d?[0-9]*_)|T0.*q(z|d?[0-9]*_))')],
    ["Static Func", re.compile('^__(TZF|T0.*FZ)')],
    ["Swift @objc Func", re.compile('^__(TTo|T0.*To$)')],
    ["Accessor", re.compile('^__(TW[atTlI]|T0.*W[atTlI]$)')],
    ["Getter/Setter", re.compile('^__(T[Fvi][gsmwWl]|T0.*f[gGsmwWal]$)')],
    ["Swift Function", re.compile('^__(TF|T0.*(F|f.|f[AuU][0-9]*_)$)')],
    ["Unknown", re.compile('')]
]


def add_function(sizes, function, start_addr, end_addr, group_by_prefix):
    if not function or start_addr is None or end_addr is None:
        return

    size = end_addr - start_addr

    if group_by_prefix:
        if function.endswith('_merged'):
            function = function[:-7]
        for cat in categories:
            cat_name = cat[0]
            pattern = cat[1]
            if pattern.match(function):
                sizes[cat_name] += size
                return
        assert False, "function name not matching any pattern"
    else:
        sizes[function] += size


def read_sizes(sect_sizes, seg_sizes, file_name, function_details,
               group_by_prefix):
    # Check if multiple architectures are supported by the object file.
    # Prefer arm64 if available.
    architectures = subprocess.check_output(
        ["otool", "-V", "-f", file_name]).decode('utf-8').split("\n")
    arch = None
    arch_pattern = re.compile(r'architecture ([\S]+)')
    for architecture in architectures:
        arch_match = arch_pattern.match(architecture)
        if arch_match:
            if arch is None:
                arch = arch_match.group(1)
            if "arm64" == arch:
                arch = "arm64"
    if arch is not None:
        cmd = ["otool", "-arch", arch]
    else:
        cmd = ["otool"]

    if function_details:
        content = subprocess.check_output(
            cmd + [
                "-l",
                "-v",
                "-t",
                file_name]).decode('utf-8').split("\n")
        content += subprocess.check_output(
            cmd + [
                "-v",
                "-s",
                "__TEXT",
                "__textcoal_nt",
                file_name]).decode('utf-8').split("\n")
    else:
        content = subprocess.check_output(
            cmd + ["-l", file_name]).decode('utf-8').split("\n")

    seg_name = None
    sect_name = None
    curr_func = None
    start_addr = None
    end_addr = None

    section_pattern = re.compile(r' +sectname ([\S]+)')
    seg_pattern = re.compile(r' +segname ([\S]+)')
    sect_size_pattern = re.compile(r' +size ([\da-fx]+)')
    seg_size_pattern = re.compile(r' +filesize ([\da-fx]+)')
    asmline_pattern = re.compile(r'^([0-9a-fA-F]+)\s')
    label_pattern = re.compile(r'^((\-*\[[^\]]*\])|[^\/\s]+):$')

    for line in content:
        asmline_match = asmline_pattern.match(line)
        if asmline_match:
            addr = int(asmline_match.group(1), 16)
            if start_addr is None:
                start_addr = addr
            end_addr = addr
        elif line == "Section":
            sect_name = None
        else:
            label_match = label_pattern.match(line)
            sect_size_match = sect_size_pattern.match(line)
            section_match = section_pattern.match(line)
            seg_match = seg_pattern.match(line)
            seg_size_match = seg_size_pattern.match(line)
            if label_match:
                func_name = label_match.group(1)
                add_function(sect_sizes, curr_func, start_addr,
                             end_addr, group_by_prefix)
                curr_func = func_name
                start_addr = None
                end_addr = None
            elif sect_size_match and sect_name and group_by_prefix:
                size = int(sect_size_match.group(1), 16)
                sect_sizes[sect_name] += size
            elif section_match:
                sect_name = section_match.group(1)
                if sect_name == "__textcoal_nt":
                    sect_name = "__text"
            elif seg_match:
                seg_name = seg_match.group(1)
            elif seg_size_match and seg_name and group_by_prefix:
                seg_size = int(seg_size_match.group(1), 16)
                seg_sizes[seg_name] += seg_size

    add_function(sect_sizes, curr_func, start_addr, end_addr, group_by_prefix)


def compare_sizes(old_sizes, new_sizes, name_key, title, total_size_key="",
                  csv=None):
    old_size = old_sizes[name_key]
    new_size = new_sizes[name_key]

    if total_size_key:
        old_total_size = old_sizes[total_size_key]
        new_total_size = new_sizes[total_size_key]

    if old_size is not None and new_size is not None:
        if old_size != 0:
            perc = "%.1f%%" % (
                (float(new_size) / float(old_size) - 1.0) * 100.0)
        else:
            perc = "- "

        if total_size_key:
            if csv:
                csv.writerow([title, name_key,
                              old_size, old_size * 100.0 / old_total_size,
                              new_size, new_size * 100.0 / new_total_size,
                              perc])
            else:
                print("%-26s%16s: %8d (%2d%%)  %8d (%2d%%)  %7s" %
                      (title, name_key,
                       old_size, old_size * 100.0 / old_total_size,
                       new_size, new_size * 100.0 / new_total_size,
                       perc))
        else:
            if csv:
                csv.writerow([title, name_key,
                              old_size, "",
                              new_size, "",
                              perc])
            else:
                print("%-26s%16s: %14d  %14d  %7s" %
                      (title, name_key, old_size, new_size, perc))


def compare_sizes_of_file(old_files, new_files, all_sections, all_segments,
                          list_categories, csv=None):
    old_sect_sizes = collections.defaultdict(int)
    new_sect_sizes = collections.defaultdict(int)
    old_seg_sizes = collections.defaultdict(int)
    new_seg_sizes = collections.defaultdict(int)

    for old_file in old_files:
        read_sizes(old_sect_sizes, old_seg_sizes, old_file, list_categories,
                   True)
    for new_file in new_files:
        read_sizes(new_sect_sizes, new_seg_sizes,
                   new_file, list_categories, True)

    if len(old_files) == 1 and len(new_files) == 1:
        old_base = os.path.basename(old_files[0])
        new_base = os.path.basename(new_files[0])
        title = old_base
        if old_base != new_base:
            title += "-" + new_base
    else:
        title = "old-new"

    compare_sizes(old_sect_sizes, new_sect_sizes, "__text", title, "", csv=csv)

    if list_categories:
        for cat in categories:
            cat_name = cat[0]
            compare_sizes(old_sect_sizes, new_sect_sizes, cat_name, "", "__text",
                          csv=csv)

    if all_sections:
        section_title = "    section"

        compare_sizes(old_sect_sizes, new_sect_sizes,
                      "__textcoal_nt", section_title, csv=csv)
        compare_sizes(old_sect_sizes, new_sect_sizes,
                      "__stubs", section_title, csv=csv)
        compare_sizes(old_sect_sizes, new_sect_sizes,
                      "__const", section_title, csv=csv)
        compare_sizes(old_sect_sizes, new_sect_sizes,
                      "__cstring", section_title, csv=csv)
        compare_sizes(old_sect_sizes, new_sect_sizes,
                      "__objc_methname", section_title, csv=csv)
        compare_sizes(old_sect_sizes, new_sect_sizes,
                      "__const", section_title, csv=csv)
        compare_sizes(old_sect_sizes, new_sect_sizes,
                      "__objc_const", section_title, csv=csv)
        compare_sizes(old_sect_sizes, new_sect_sizes,
                      "__data", section_title, csv=csv)
        compare_sizes(old_sect_sizes, new_sect_sizes,
                      "__swift5_proto", section_title, csv=csv)
        compare_sizes(old_sect_sizes, new_sect_sizes,
                      "__common", section_title, csv=csv)
        compare_sizes(old_sect_sizes, new_sect_sizes,
                      "__bss", section_title, csv=csv)

    if all_segments:
        segment_title = "    segment"
        compare_sizes(old_seg_sizes, new_seg_sizes, "__TEXT", segment_title,
                      csv=csv)
        compare_sizes(old_seg_sizes, new_seg_sizes, "__DATA", segment_title,
                      csv=csv)
        compare_sizes(old_seg_sizes, new_seg_sizes, "__LLVM_COV", segment_title,
                      csv=csv)
        compare_sizes(old_seg_sizes, new_seg_sizes, "__LINKEDIT", segment_title,
                      csv=csv)


def list_function_sizes(size_array):
    for pair in sorted(size_array, key=itemgetter(1)):
        name = pair[0]
        size = pair[1]
        yield "%8d %s" % (size, name)


def compare_function_sizes(old_files, new_files, csv=None):
    old_sizes = collections.defaultdict(int)
    new_sizes = collections.defaultdict(int)
    for name in old_files:
        read_sizes(old_sizes, [], name, True, False)
    for name in new_files:
        read_sizes(new_sizes, [], name, True, False)

    only_in_file1 = []
    only_in_file2 = []
    in_both = []

    only_in_file1size = 0
    only_in_file2size = 0
    in_both_size = 0

    for func, old_size in old_sizes.items():
        new_size = new_sizes[func]
        if new_size != 0:
            in_both.append((func, old_size, new_size))
        else:
            only_in_file1.append((func, old_size))
            only_in_file1size += old_size

    for func, new_size in new_sizes.items():
        old_size = old_sizes[func]
        if old_size == 0:
            only_in_file2.append((func, new_size))
            only_in_file2size += new_size

    if only_in_file1:
        if csv:
            csv.writerow(["Only in old", "", "", ""])
            for name, size in sorted(only_in_file1, key=itemgetter(1)):
                csv.writerow([size, name, "", ""])
            csv.writerow(["Total size only in old", only_in_file1size, "", ""])
        else:
            print("Only in old file(s)")
            print(os.linesep.join(list_function_sizes(only_in_file1)))
            print("Total size of functions only in old file: {}".format(
                only_in_file1size))
            print()

    if only_in_file2:
        if csv:
            csv.writerow(["Only in new", "", "", ""])
            for name, size in sorted(only_in_file2, key=itemgetter(1)):
                csv.writerow([size, name, "", ""])
            csv.writerow(["Total size only in new", only_in_file2size, "", ""])
        else:
            print("Only in new files(s)")
            print(os.linesep.join(list_function_sizes(only_in_file2)))
            print("Total size of functions only in new file: {}".format(
                only_in_file2size))
            print()

    if in_both:
        size_increase = 0
        size_decrease = 0

        header = ("old", "new", "diff")
        if csv:
            csv.writerow(list(header) + ["function"])
        else:
            print("%8s %8s %8s" % header)

        for triple in sorted(
                in_both,
                key=lambda tup: (tup[2] - tup[1], tup[1])):
            func = triple[0]
            old_size = triple[1]
            new_size = triple[2]
            diff = new_size - old_size

            if diff > 0:
                size_increase += diff
            else:
                size_decrease -= diff
            if diff == 0:
                in_both_size += new_size

            if csv:
                csv.writerow([old_size, new_size, new_size - old_size, func])
            else:
                print("%8d %8d %8d %s" %
                      (old_size, new_size, new_size - old_size, func))

        if csv:
            csv.writerow(["Total size in both", "Total size smaller",
                          "Total size bigger", "Total size change in both"])
            csv.writerow([in_both_size, size_decrease, size_increase,
                          (size_increase - size_decrease)])
        else:
            print("Total size of functions " +
                  "with the same size in both files: {}".format(in_both_size))
            print("Total size of functions " +
                  "that got smaller: {}".format(size_decrease))
            print("Total size of functions " +
                  "that got bigger: {}".format(size_increase))
            print("Total size change of functions present " +
                  "in both files: {}".format(size_increase - size_decrease))
