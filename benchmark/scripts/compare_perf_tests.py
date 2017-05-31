#!/usr/bin/python
# -*- coding: utf-8 -*-

# ===--- compare_perf_tests.py -------------------------------------------===//
#
#  This source file is part of the Swift.org open source project
#
#  Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
#  Licensed under Apache License v2.0 with Runtime Library Exception
#
#  See https://swift.org/LICENSE.txt for license information
#  See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ===---------------------------------------------------------------------===//

from __future__ import print_function

import argparse
import csv
import re
import sys
from math import sqrt


class PerformanceTestResult(object):
    """PerformanceTestResult holds results from executing individual benchmark
    from Swift Benchmark Suite as reported by test driver (Benchmark_O,
    Benchmark_Onone, Benchmark_Ounchecked or Benchmark_Driver).

    It depends on the log format emmited by the test driver in the form:
    #,TEST,SAMPLES,MIN(μs),MAX(μs),MEAN(μs),SD(μs),MEDIAN(μs),MAX_RSS(B)

    The last column, MAX_RSS, is emmited only for runs instrumented by the
    Benchmark_Driver to measure rough memory use during the execution of the
    benchmark.
    """
    def __init__(self, csv_row):
        """PerformanceTestResult instance is created from an iterable with
        length of 8 or 9. (Like a row provided by the CSV parser.)
        """
        # csv_row[0] is just an ordinal number of the test - skip that
        self.name = csv_row[1]          # Name of the performance test
        self.samples = int(csv_row[2])  # Number of measurement samples taken
        self.min = int(csv_row[3])      # Minimum runtime (ms)
        self.max = int(csv_row[4])      # Maximum runtime (ms)
        self.mean = int(csv_row[5])     # Mean (average) runtime (ms)
        sd = int(csv_row[6])            # Standard Deviation (ms)
        # For computing running variance
        self.S_runtime = (0 if self.samples < 2 else
                          (sd * sd) * (self.samples - 1))
        self.median = int(csv_row[7])   # Median runtime (ms)
        self.max_rss = (                # Maximum Resident Set Size (B)
            int(csv_row[8]) if len(csv_row) > 8 else None)

    @property
    def sd(self):
        """Standard Deviation (ms)"""
        return (0 if self.samples < 2 else
                sqrt(self.S_runtime / (self.samples - 1)))

    @staticmethod
    def running_mean_variance((k, M_, S_), x):
        """
        Compute running variance, B. P. Welford's method
        See Knuth TAOCP vol 2, 3rd edition, page 232, or
        https://www.johndcook.com/blog/standard_deviation/
        M is mean, Standard Deviation is defined as sqrt(S/k-1)
        """
        k = float(k + 1)
        M = M_ + (x - M_) / k
        S = S_ + (x - M_) * (x - M)
        return (k, M, S)

    def merge(self, r):
        """Merging test results recomputes min and max.
        It attempts to recompute mean and standard deviation when all_samples
        are available. There is no correct way to compute these values from
        test results that are summaries from more than 3 samples.

        The use case here is comparing tests results parsed from concatenated
        log files from multiple runs of benchmark driver.
        """
        self.min = min(self.min, r.min)
        self.max = max(self.max, r.max)
        # self.median = None # unclear what to do here

        def push(x):
            state = (self.samples, self.mean, self.S_runtime)
            state = self.running_mean_variance(state, x)
            (self.samples, self.mean, self.S_runtime) = state

        # Merging test results with up to 3 samples is exact
        values = [r.min, r.max, r.median][:min(r.samples, 3)]
        map(push, values)

    # Column labels for header row in results table
    header = ('TEST', 'MIN', 'MAX', 'MEAN', 'MAX_RSS')

    def values(self, break_words=False):
        """Values property for display in results table comparisons
        in format: ('TEST', 'MIN', 'MAX', 'MEAN', 'MAX_RSS').
        Test name can have invisible breaks inserted into camel case word
        boundaries to prevent overly long tables when displayed on GitHub.
        The `break_words` attribute controls this behavior, it is off by
        default.
        """
        return (
            break_word_camel_case(self.name) if break_words else self.name,
            str(self.min), str(self.max), str(int(self.mean)),
            str(self.max_rss) if self.max_rss else '—'
        )


class ResultComparison(object):
    """ResultComparison compares MINs from new and old PerformanceTestResult.
    It computes speedup ratio and improvement delta (%).
    """
    def __init__(self, old, new):
        self.old = old
        self.new = new
        assert(old.name == new.name)
        self.name = old.name  # Test name, convenience accessor

        # Speedup ratio
        self.ratio = (old.min + 0.001) / (new.min + 0.001)

        # Test runtime improvement in %
        ratio = (new.min + 0.001) / (old.min + 0.001)
        self.delta = ((ratio - 1) * 100)

        # Add ' (?)' to the speedup column as indication of dubious changes:
        # result's MIN falls inside the (MIN, MAX) interval of result they are
        # being compared with.
        self.is_dubious = (
            ' (?)' if ((old.min < new.min and new.min < old.max) or
                       (new.min < old.min and old.min < new.max))
            else '')

    # Column labels for header row in results table
    header = ('TEST', 'OLD', 'NEW', 'DELTA', 'SPEEDUP')

    def values(self, break_words=False):
        """Values property for display in results table comparisons
        in format: ('TEST', 'OLD', 'NEW', 'DELTA', 'SPEEDUP').
        Test name can have invisible breaks inserted into camel case word
        boundaries to prevent overly long tables when displayed on GitHub.
        The `break_words` attribute controls this behavior, it is off by
        default.
        """
        return (break_word_camel_case(self.name) if break_words else self.name,
                str(self.old.min), str(self.new.min),
                '{0:+.1f}%'.format(self.delta),
                '{0:.2f}x{1}'.format(self.ratio, self.is_dubious))


class TestComparator(object):
    def __init__(self, old_file, new_file, delta_threshold):

        def load_from_CSV(filename):  # handles output from Benchmark_O and
            def skip_totals(row):     # Benchmark_Driver (added MAX_RSS column)
                return len(row) > 7 and row[0].isdigit()
            tests = map(PerformanceTestResult,
                        filter(skip_totals, csv.reader(open(filename))))

            def add_or_merge(names, r):
                if r.name not in names:
                    names[r.name] = r
                else:
                    names[r.name].merge(r)
                return names
            return reduce(add_or_merge, tests, dict())

        old_results = load_from_CSV(old_file)
        new_results = load_from_CSV(new_file)
        old_tests = set(old_results.keys())
        new_tests = set(new_results.keys())
        comparable_tests = new_tests.intersection(old_tests)
        added_tests = new_tests.difference(old_tests)
        removed_tests = old_tests.difference(new_tests)

        self.added = sorted([new_results[t] for t in added_tests],
                            key=lambda r: r.name)
        self.removed = sorted([old_results[t] for t in removed_tests],
                              key=lambda r: r.name)

        def compare(name):
            return ResultComparison(old_results[name], new_results[name])

        comparisons = map(compare, comparable_tests)

        def partition(l, p):
            return reduce(lambda x, y: x[not p(y)].append(y) or x, l, ([], []))

        decreased, not_decreased = partition(
            comparisons, lambda c: c.ratio < (1 - delta_threshold))
        increased, unchanged = partition(
            not_decreased, lambda c: c.ratio > (1 + delta_threshold))

        # sorted partitions
        names = [c.name for c in comparisons]
        comparisons = dict(zip(names, comparisons))
        self.decreased = [comparisons[c.name]
                          for c in sorted(decreased, key=lambda c: -c.delta)]
        self.increased = [comparisons[c.name]
                          for c in sorted(increased, key=lambda c: c.delta)]
        self.unchanged = [comparisons[c.name]
                          for c in sorted(unchanged, key=lambda c: c.name)]


class ReportFormatter(object):
    def __init__(self, comparator, old_branch, new_branch, changes_only):
        self.comparator = comparator
        self.old_branch = old_branch
        self.new_branch = new_branch
        self.changes_only = changes_only
        self.break_words = False

    MARKDOWN_DETAIL = u"""
<details {3}>
  <summary>{0} ({1})</summary>
  {2}
</details>
"""
    GIT_DETAIL = u"""
{0} ({1}): {2}"""

    def markdown(self):
        self.break_words = True
        return self._formatted_text(
            ROW=u'{0} | {1} | {2} | {3} | {4} \n',
            HEADER_SEPARATOR=u'---',
            DETAIL=self.MARKDOWN_DETAIL)

    def git(self):
        return self._formatted_text(
            ROW=u'{0}   {1}   {2}   {3}   {4} \n',
            HEADER_SEPARATOR=u'   ',
            DETAIL=self.GIT_DETAIL)

    def _column_widths(self):
        changed = self.comparator.decreased + self.comparator.increased
        comparisons = (changed if self.changes_only else
                       changed + self.comparator.unchanged)
        comparisons += self.comparator.added + self.comparator.removed

        values = [c.values(break_words=self.break_words)
                  for c in comparisons]
        widths = map(lambda columns: map(len, columns),
                     [PerformanceTestResult.header, ResultComparison.header] +
                     values)

        def max_widths(maximum, widths):
            return tuple(map(max, zip(maximum, widths)))

        return reduce(max_widths, widths, tuple([0] * 5))

    def _formatted_text(self, ROW, HEADER_SEPARATOR, DETAIL):
        widths = self._column_widths()

        def justify_columns(contents):
            return tuple(map(lambda (w, c): c.ljust(w), zip(widths, contents)))

        def row(contents):
            return ROW.format(*justify_columns(contents))

        def header(header):
            return '\n' + row(header) + row(tuple([HEADER_SEPARATOR] * 5))

        def format_columns(r, strong):
            return (r if not strong else
                    r[:-1] + ('**{0}**'.format(r[-1]), ))

        def table(title, results, is_strong=False, is_open=False):
            rows = [
                row(format_columns(
                    result_comparison.values(break_words=self.break_words),
                    is_strong))
                for result_comparison in results
            ]
            return ('' if not rows else
                    DETAIL.format(*[
                        title, len(results),
                        (header(results[0].header) + ''.join(rows)),
                        ('open' if is_open else '')
                    ]))

        return ''.join([
            # FIXME print self.old_branch, self.new_branch
            table('Regression', self.comparator.decreased, True, True),
            table('Improvement', self.comparator.increased, True),
            ('' if self.changes_only else
             table('No Changes', self.comparator.unchanged)),
            table('Added', self.comparator.added, is_open=True),
            table('Removed', self.comparator.removed, is_open=True)
        ]).encode('utf-8')

    HTML = u"""
<!DOCTYPE html>
<html>
<head>
    <meta http-equiv="Content-Type" content="text/html; charset=utf-8" />
    <style>
        body {{ font-family: -apple-system, sans-serif; font-size: 14px; }}
        table {{ border-spacing: 2px; border-color: gray; border-spacing: 0;
                border-collapse: collapse; }}
        table tr {{ background-color: #fff; border-top: 1px solid #c6cbd1; }}
        table th, table td {{ padding: 6px 13px; border: 1px solid #dfe2e5; }}
        th {{ text-align: center; padding-top: 130px; }}
        td {{ text-align: right; }}
        table td:first-child {{ text-align: left; }}
        tr:nth-child(even) {{ background-color: #000000; }}
        tr:nth-child(2n) {{ background-color: #f6f8fa; }}
    </style>
</head>
<body>
<table>
{0}
</table>
</body>
</html>"""

    HTML_HEADER_ROW = u"""
        <tr>
                <th align='left'>{0} ({1})</th>
                <th align='left'>{2}</th>
                <th align='left'>{3}</th>
                <th align='left'>{4}</th>
                <th align='left'>{5}</th>
        </tr>
"""

    HTML_ROW = u"""
        <tr>
                <td align='left'>{0}</td>
                <td align='left'>{1}</td>
                <td align='left'>{2}</td>
                <td align='left'>{3}</td>
                <td align='left'><font color='{4}'>{5}</font></td>
        </tr>
"""

    def html(self):

        def row(name, old, new, delta, speedup, speedup_color):
            return self.HTML_ROW.format(
                name, old, new, delta, speedup_color, speedup)

        def header(contents):
            return self.HTML_HEADER_ROW.format(* contents)

        def table(title, results, speedup_color):
            rows = [
                row(*(result_comparison.values() + (speedup_color,)))
                for result_comparison in results
            ]
            return ('' if not rows else
                    header((title, len(results)) + results[0].header[1:]) +
                    ''.join(rows))

        return self.HTML.format(
            ''.join([
                # FIXME print self.old_branch, self.new_branch
                table('Regression', self.comparator.decreased, 'red'),
                table('Improvement', self.comparator.increased, 'green'),
                ('' if self.changes_only else
                 table('No Changes', self.comparator.unchanged, 'black')),
                table('Added', self.comparator.added, ''),
                table('Removed', self.comparator.removed, '')
            ])).encode('utf-8')


def main():

    parser = argparse.ArgumentParser(description='Compare Performance tests.')
    parser.add_argument('--old-file',
                        help='Baseline performance test suite (csv file)',
                        required=True)
    parser.add_argument('--new-file',
                        help='New performance test suite (csv file)',
                        required=True)
    parser.add_argument('--format',
                        choices=['markdown', 'git', 'html'],
                        help='Output format. Default is markdown.',
                        default="markdown")
    parser.add_argument('--output', help='Output file name')
    parser.add_argument('--changes-only',
                        help='Output only affected tests', action='store_true')
    parser.add_argument('--new-branch',
                        help='Name of the new branch', default='NEW_MIN')
    parser.add_argument('--old-branch',
                        help='Name of the old branch', default='OLD_MIN')
    parser.add_argument('--delta-threshold',
                        help='Delta threshold. Default 0.05.', default='0.05')

    args = parser.parse_args()
    comparator = TestComparator(args.old_file, args.new_file,
                                float(args.delta_threshold))
    formatter = ReportFormatter(comparator, args.old_branch, args.new_branch,
                                args.changes_only)

    if args.format:
        if args.format.lower() != 'markdown':
            print(formatter.git())
        else:
            print(formatter.markdown())

    if args.format:
        if args.format.lower() == 'html':
            if args.output:
                write_to_file(args.output, formatter.html())
            else:
                print('Error: missing --output flag.')
                sys.exit(1)
        elif args.format.lower() == 'markdown':
            if args.output:
                write_to_file(args.output, formatter.markdown())
        elif args.format.lower() != 'git':
            print('{0} is unknown format.'.format(args.format))
            sys.exit(1)


FIRST_CAP_RE = re.compile('(.)([A-Z][a-z]+)')
ALL_CAP_RE = re.compile('([a-z0-9])([A-Z])')


def break_word_camel_case(name, separator='\xe2\x80\x8b'):
    """Insert sperator between words in the camelCased name.
    Default separator is zero-width space (U+200B).
    """
    replacement = r'\1' + separator + r'\2'
    name = FIRST_CAP_RE.sub(replacement, name)
    name = ALL_CAP_RE.sub(replacement, name)
    return unicode(name, encoding='utf-8')


def write_to_file(file_name, data):
    """
    Write data to given file
    """
    output_file = open(file_name, 'w')
    output_file.write(data)
    output_file.close


if __name__ == '__main__':
    sys.exit(main())
