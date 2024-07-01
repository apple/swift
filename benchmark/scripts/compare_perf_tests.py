#!/usr/bin/env python3
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
"""
This script compares performance test logs and issues a formatted report.

Invoke `$ compare_perf_tests.py -h ` for complete list of options.

class `PerformanceTestResult` collects information about a single test
class `LogParser` converts log files into `PerformanceTestResult`s.
class `ResultComparison` compares new and old `PerformanceTestResult`s.
class `TestComparator` analyzes changes between the old and new test results.
class `ReportFormatter` creates the test comparison report in specified format.

"""

import argparse
import functools
import json
import re
import statistics
import sys


class PerformanceTestResult(object):
    u"""Result from executing an individual Swift Benchmark Suite benchmark.

    Reported by the test driver (Benchmark_O, Benchmark_Onone, Benchmark_Osize
    or Benchmark_Driver).

    It supports  log formats emitted by the test driver.
    """

    # TODO: Delete after December 2023
    @classmethod
    def fromOldFormat(cls, header, line):
        """Original format with statistics for normal distribution (MEAN, SD):
             #,TEST,SAMPLES,MIN(μs),MAX(μs),MEAN(μs),SD(μs),MEDIAN(μs),MAX_RSS(B),PAGES,ICS,YIELD
           Note that MAX_RSS, PAGES, ICS, YIELD are all optional
        """
        csv_row = line.split(",") if "," in line else line.split()
        labels = header.split(",") if "," in header else header.split()

        # Synthesize a JSON form with the basic values:
        num_samples = int(csv_row[2])
        json_data = {
            "number": int(csv_row[0]),
            "name": csv_row[1],
            "num_samples": num_samples,
        }

        # Map remaining columns according to label
        field_map = [
            ("ICS", "ics"),
            ("MAX_RSS", "max_rss"),  # Must precede "MAX"
            ("MAX", "max"),
            ("MEAN", "mean"),
            ("MEDIAN", "median"),
            ("MIN", "min"),
            ("PAGES", "pages"),
            ("SD", "sd"),
            ("YIELD", "yield")
        ]
        for label, value in zip(labels, csv_row):
            for match, json_key in field_map:
                if match in label:
                    json_data[json_key] = float(value)
                    break

        # Heroic: Reconstruct samples if we have enough info
        # This is generally a bad idea, but sadly necessary for the
        # old format that doesn't provide raw sample data.
        if num_samples == 1 and "min" in json_data:
            json_data["samples"] = [
                json_data["min"]
            ]
        elif num_samples == 2 and "min" in json_data and "max" in json_data:
            json_data["samples"] = [
                json_data["min"],
                json_data["max"]
            ]
        elif (num_samples == 3
              and "min" in json_data
              and "max" in json_data
              and "median" in json_data):
            json_data["samples"] = [
                json_data["min"],
                json_data["median"],
                json_data["max"]
            ]

        return PerformanceTestResult(json_data)

    # TODO: Delete after December 2023
    @classmethod
    def fromQuantileFormat(cls, header, line):
        """Quantiles format with variable number of columns depending on the
           number of quantiles:
           #,TEST,SAMPLES,QMIN(μs),MEDIAN(μs),MAX(μs)
           #,TEST,SAMPLES,QMIN(μs),Q1(μs),Q2(μs),Q3(μs),MAX(μs),MAX_RSS(B)
        The number of columns between QMIN and MAX depends on the test driver's
        `--quantile`parameter. In both cases, the last column, MAX_RSS is optional.

        Delta encoding: If a header name includes 𝚫, that column stores the
        difference from the previous column.  E.g, a header
        "#,TEST,SAMPLES,QMIN(μs),MEDIAN(μs),𝚫MAX(μs)" indicates the final "MAX"
        column must be computed by adding the value in that column to the value
        of the previous "MEDIAN" column.
        """
        csv_row = line.split(",") if "," in line else line.split()
        labels = header.split(",")

        for i in range(1, len(labels)):
            if "𝚫" in labels[i] or "Δ" in labels[i]:
                prev = int(csv_row[i - 1])
                inc = int(csv_row[i]) if csv_row[i] != '' else 0
                csv_row[i] = str(prev + inc)

        # Synthesize a JSON form and then initialize from that
        json_data = {
            "number": int(csv_row[0]),
            "name": csv_row[1],
            "num_samples": int(csv_row[2]),
        }
        # Process optional trailing fields MAX_RSS, PAGES, ICS, YIELD
        i = len(labels) - 1
        while True:
            if "MAX_RSS" in labels[i]:
                json_data["max_rss"] = float(csv_row[i])
            elif "PAGES" in labels[i]:
                json_data["pages"] = float(csv_row[i])
            elif "ICS" in labels[i]:
                json_data["ics"] = float(csv_row[i])
            elif "YIELD" in labels[i]:
                json_data["yield"] = float(csv_row[i])
            else:
                break
            i -= 1
            if i < 0:
                break

        # Rest is the quantiles (includes min/max columns)
        quantiles = [float(q) for q in csv_row[3:i + 1]]

        # Heroic effort:
        # If we have enough quantiles, we can reconstruct the samples
        # This is generally a bad idea, but sadly necessary since
        # the quantile format doesn't provide raw sample data.
        if json_data["num_samples"] == len(quantiles):
            json_data["samples"] = sorted(quantiles)
        elif json_data["num_samples"] == 2:
            json_data["samples"] = [quantiles[0], quantiles[-1]]
        elif json_data["num_samples"] == 1:
            json_data["samples"] = [quantiles[0]]
        else:
            json_data["quantiles"] = quantiles
        if len(quantiles) > 0:
            json_data["min"] = quantiles[0]
            json_data["max"] = quantiles[-1]
            json_data["median"] = quantiles[(len(quantiles) - 1) // 2]

        return PerformanceTestResult(json_data)

    @classmethod
    def fromJSONFormat(cls, line):
        """JSON format stores a test result as a JSON object on a single line

        Compared to the legacy tab-separated/comma-separated formats, this makes
        it much easier to add new fields, handle optional fields, and allows us
        to include the full set of samples so we can use better statistics
        downstream.

        The code here includes optional support for min, max,
        median, mean, etc. supported by the older formats, though in practice,
        you shouldn't rely on those:  Just store the full samples and then
        compute whatever statistics you need as required.
        """
        json_data = json.loads(line)
        return PerformanceTestResult(json_data)

    def __init__(self, json_data):
        # Ugly hack to get the old tests to run
        if isinstance(json_data, str):
            json_data = json.loads(json_data)

        # We always have these
        assert (json_data.get("number") is not None)
        assert (json_data.get("name") is not None)
        self.test_num = json_data["number"]
        self.name = json_data["name"]

        # We always have either samples or num_samples
        assert (json_data.get("num_samples") is not None
                or json_data.get("samples") is not None)
        self.num_samples = json_data.get("num_samples") or len(json_data["samples"])
        self.samples = json_data.get("samples") or []

        # Everything else is optional and can be read
        # out of the JSON data if needed
        # See max_rss() below for an example of this.
        self.json_data = dict(json_data)

    def __repr__(self):
        return "PerformanceTestResult(" + json.dumps(self.json_data) + ")"

    def json(self):
        """Return a single-line JSON form of this result

        This can be parsed back via fromJSONFormat above.
        It can also represent all data stored by the older
        formats, so there's no reason to not use it everywhere.
        """
        data = dict(self.json_data)

        # In case these got modified
        data["number"] = self.test_num
        data["name"] = self.name

        # If we have full sample data, use that and
        # drop any lingering pre-computed statistics
        # (It's better for downstream consumers to just
        # compute whatever statistics they need from scratch.)

        # After December 2023, uncomment the next line:
        # assert len(self.samples) == self.num_samples
        if len(self.samples) == self.num_samples:
            data["samples"] = self.samples
            data.pop("num_samples", None)
            # TODO: Delete min/max/mean/sd/q1/median/q3/quantiles
            # after December 2023
            data.pop("min", None)
            data.pop("max", None)
            data.pop("mean", None)
            data.pop("sd", None)
            data.pop("q1", None)
            data.pop("median", None)
            data.pop("q3", None)
            data.pop("quantiles", None)
        else:
            # Preserve other pre-existing JSON statistics
            data["num_samples"] = self.num_samples

        return json.dumps(data)

    def __str__(self):
        return self.json()

    @property
    def setup(self):
        """TODO: Implement this
        """
        return 0

    @property
    def max_rss(self):
        """Return max_rss if available
        """
        return self.json_data.get("max_rss")

    @property
    def mem_pages(self):
        """Return pages if available
        """
        return self.json_data.get("pages")

    @property
    def involuntary_cs(self):
        """Return involuntary context switches if available
        """
        return self.json_data.get("ics")

    @property
    def yield_count(self):
        """Return voluntary yield count if available
        """
        return self.json_data.get("yield")

    @property
    def min_value(self):
        """Return the minimum value from all samples

        If we have full samples, compute it directly.
        In the legacy case, we might not have full samples,
        so in that case we'll return a value that was given
        to us initially (if any).

        Eventually (after December 2023), this can be simplified
        to just `return min(self.samples)`, since by then
        the legacy forms should no longer be in use.
        """
        if self.num_samples == len(self.samples):
            return min(self.samples)
        return self.json_data.get("min")

    @property
    def max_value(self):
        """Return the maximum sample value

        See min_value comments for details on the legacy behavior."""
        if self.num_samples == len(self.samples):
            return max(self.samples)
        return self.json_data.get("max")

    @property
    def median(self):
        """Return the median sample value

        See min_value comments for details on the legacy behavior."""
        if self.num_samples == len(self.samples):
            return statistics.median(self.samples)
        return self.json_data.get("median")

    # TODO: Eliminate q1 and q3.  They're kept for now
    # to preserve compatibility with older reports.  But quantiles
    # aren't really useful statistics, so just drop them.
    @property
    def q1(self):
        """Return the 25% quantile

        See min_value comments for details on the legacy behavior."""
        if self.num_samples == len(self.samples):
            q = statistics.quantiles(self.samples, n=4)
            return q[0]
        return self.json_data.get("q1")

    @property
    def q3(self):
        """Return the 75% quantile

        See min_value comments for details on the legacy behavior."""
        if self.num_samples == len(self.samples):
            q = statistics.quantiles(self.samples, n=4)
            return q[2]
        return self.json_data.get("q3")

    @property
    def mean(self):
        """Return the average

        TODO: delete this; it's not useful"""
        if self.num_samples == len(self.samples):
            return statistics.mean(self.samples)
        return self.json_data.get("mean")

    @property
    def sd(self):
        """Return the standard deviation

        TODO: delete this; it's not useful"""
        if self.num_samples == len(self.samples):
            if len(self.samples) > 1:
                return statistics.stdev(self.samples)
            else:
                return 0
        return self.json_data.get("sd")

    def merge(self, other):
        """Merge two results.

        This is trivial in the non-legacy case:  We just
        pool all the samples.

        In the legacy case (or the mixed legacy/non-legacy cases),
        we try to estimate the min/max/mean/sd/median/etc based
        on whatever information is available.  After Dec 2023,
        we should be able to drop the legacy support.
        """
        # The following can be removed after Dec 2023
        # (by which time the legacy support should no longer
        # be necessary)
        if self.num_samples != len(self.samples):
            # If we don't have samples, we can't rely on being
            # able to compute real statistics from those samples,
            # so we make a best-effort attempt to estimate a joined
            # statistic from whatever data we actually have.

            # If both exist, take the minimum, else take whichever is set
            other_min_value = other.min_value
            if other_min_value is not None:
                self_min_value = self.min_value
                if self_min_value is not None:
                    self.json_data["min"] = min(other_min_value, self_min_value)
                else:
                    self.json_data["min"] = other_min_value

            # If both exist, take the maximum, else take whichever is set
            other_max_value = other.max_value
            if other_max_value is not None:
                self_max_value = self.max_value
                if self_max_value is not None:
                    self.json_data["max"] = max(other_max_value, self_max_value)
                else:
                    self.json_data["max"] = other_max_value

            # If both exist, take the weighted average, else take whichever is set
            other_mean = other.mean
            if other_mean is not None:
                self_mean = self.mean
                if self_mean is not None:
                    self.json_data["mean"] = (
                        (other_mean * other.num_samples
                         + self_mean * self.num_samples)
                        / (self.num_samples + other.num_samples)
                    )
                else:
                    self.json_data["mean"] = other_mean
            self.json_data.pop("median", None)  # Remove median
            self.json_data.pop("sd", None)  # Remove stdev
            self.json_data.pop("q1", None)  # Remove 25% quantile
            self.json_data.pop("q3", None)  # Remove 75% quantile
            self.json_data.pop("quantiles", None)  # Remove quantiles

        # Accumulate samples (if present) and num_samples (always)
        self.samples += other.samples
        self.num_samples += other.num_samples

        # Metadata
        # Use the smaller if both have a max_rss value
        self.json_data["max_rss"] = other.max_rss
        other_max_rss = other.max_rss
        if other_max_rss is not None:
            self_max_rss = self.max_rss
            if self_max_rss is not None:
                self.json_data["max_rss"] = min(self_max_rss, other_max_rss)
            else:
                self.json_data["max_rss"] = other_max_rss


class ResultComparison(object):
    """ResultComparison compares MINs from new and old PerformanceTestResult.

    It computes speedup ratio and improvement delta (%).
    """

    def __init__(self, old, new):
        """Initialize with old and new `PerformanceTestResult`s to compare."""
        self.old = old
        self.new = new
        assert old.name == new.name
        self.name = old.name  # Test name, convenience accessor

        # Speedup ratio
        self.ratio = (old.min_value + 0.001) / (new.min_value + 0.001)

        # Test runtime improvement in %
        ratio = (new.min_value + 0.001) / (old.min_value + 0.001)
        self.delta = (ratio - 1) * 100

        # If we have full samples for both old and new...
        if (
                len(old.samples) == old.num_samples
                and len(new.samples) == new.num_samples
        ):
            # TODO: Use a T-Test or U-Test to determine whether
            # one set of samples should be considered reliably better than
            # the other.
            None

        # If we do not have full samples, we'll use the
        # legacy calculation for compatibility.
        # TODO: After Dec 2023, we should always be using full samples
        # everywhere and can delete the following entirely.
        #
        # Indication of dubious changes: when result's MIN falls inside the
        # (MIN, MAX) interval of result they are being compared with.
        self.is_dubious = (
            (
                old.min_value < new.min_value
                and new.min_value < old.max_value
            ) or (
                new.min_value < old.min_value
                and old.min_value < new.max_value
            )
        )


class LogParser(object):
    """Converts log outputs into `PerformanceTestResult`s.

    Supports various formats produced by the `Benchmark_Driver` and
    `Benchmark_O`('Onone', 'Osize'). It can also merge together the
    results from concatenated log files.
    """

    def __init__(self):
        """Create instance of `LogParser`."""
        self.results = []

    def parse_results(self, lines):
        """Parse results from the lines of the log output from Benchmark*.

        Returns a list of `PerformanceTestResult`s.
        """
        match_json = re.compile(r"\s*({.*)")
        match_header = re.compile(r"( *#[, \t]+TEST.*)")
        match_legacy = re.compile(r" *(\d+[, \t].*)")
        header = ""
        for line in lines:
            # Current format has a JSON-encoded object on each line
            # That format is flexible so should be the only format
            # used going forward
            if match_json.match(line):
                r = PerformanceTestResult.fromJSONFormat(line)
                self.results.append(r)
            elif match_header.match(line):
                # Legacy formats use a header line (which can be
                # inspected to determine the presence and order of columns)
                header = line
            elif match_legacy.match(line):
                # Legacy format: lines of space- or tab-separated values
                if "QMIN" in header:
                    r = PerformanceTestResult.fromQuantileFormat(header, line)
                else:
                    r = PerformanceTestResult.fromOldFormat(header, line)
                self.results.append(r)
            else:
                # Ignore unrecognized lines
                # print('Skipping: ' + line.rstrip('\n'), file=sys.stderr, flush=True)
                continue
        return self.results

    @staticmethod
    def _results_from_lines(lines):
        names = dict()
        for r in LogParser().parse_results(lines):
            if r.name not in names:
                names[r.name] = r
            else:
                names[r.name].merge(r)
        return names

    @staticmethod
    def results_from_string(log_contents):
        """Parse `PerformanceTestResult`s from the supplied string.

        Returns dictionary of test names and `PerformanceTestResult`s.
        """
        return LogParser._results_from_lines(log_contents.splitlines())

    @staticmethod
    def results_from_file(log_file):
        """Parse `PerformanceTestResult`s from the log file.

        Returns dictionary of test names and `PerformanceTestResult`s.
        """
        with open(log_file) as f:
            return LogParser._results_from_lines(f.readlines())


class TestComparator(object):
    """Analyzes changes between the old and new test results.

    It determines which tests were `added`, `removed` and which can be
    compared. It then splits the `ResultComparison`s into 3 groups according to
    the `delta_threshold` by the change in performance: `increased`,
    `decreased` and `unchanged`. Whole computation is performed during
    initialization and results are provided as properties on this object.

    The lists of `added`, `removed` and `unchanged` tests are sorted
    alphabetically. The `increased` and `decreased` lists are sorted in
    descending order by the amount of change.
    """

    def __init__(self, old_results, new_results, delta_threshold):
        """Initialize with dictionaries of old and new benchmark results.

        Dictionary keys are benchmark names, values are
        `PerformanceTestResult`s.
        """
        old_tests = set(old_results.keys())
        new_tests = set(new_results.keys())
        comparable_tests = new_tests.intersection(old_tests)
        added_tests = new_tests.difference(old_tests)
        removed_tests = old_tests.difference(new_tests)

        self.added = sorted([new_results[t] for t in added_tests], key=lambda r: r.name)
        self.removed = sorted(
            [old_results[t] for t in removed_tests], key=lambda r: r.name
        )

        def compare(name):
            return ResultComparison(old_results[name], new_results[name])

        comparisons = list(map(compare, comparable_tests))

        def partition(items, p):
            return functools.reduce(
                lambda x, y: x[not p(y)].append(y) or x, items, ([], [])
            )

        decreased, not_decreased = partition(
            comparisons, lambda c: c.ratio < (1 - delta_threshold)
        )
        increased, unchanged = partition(
            not_decreased, lambda c: c.ratio > (1 + delta_threshold)
        )

        # sorted partitions
        names = [c.name for c in comparisons]
        comparisons = dict(zip(names, comparisons))
        self.decreased = [
            comparisons[c.name] for c in sorted(decreased, key=lambda c: -c.delta)
        ]
        self.increased = [
            comparisons[c.name] for c in sorted(increased, key=lambda c: c.delta)
        ]
        self.unchanged = [
            comparisons[c.name] for c in sorted(unchanged, key=lambda c: c.name)
        ]


class ReportFormatter(object):
    """Creates the report from performance test comparison in specified format.

    `ReportFormatter` formats the `PerformanceTestResult`s and
    `ResultComparison`s provided by `TestComparator` into report table.
    Supported formats are: `markdown` (used for displaying benchmark results on
    GitHub), `git` and `html`.
    """

    def __init__(self, comparator, changes_only, single_table=False):
        """Initialize with `TestComparator` and names of branches."""
        self.comparator = comparator
        self.changes_only = changes_only
        self.single_table = single_table

    PERFORMANCE_TEST_RESULT_HEADER = ("TEST", "MIN", "MAX", "MEAN", "MAX_RSS")
    RESULT_COMPARISON_HEADER = ("TEST", "OLD", "NEW", "DELTA", "RATIO")

    @staticmethod
    def header_for(result):
        """Column labels for header row in results table."""
        return (
            ReportFormatter.PERFORMANCE_TEST_RESULT_HEADER
            if isinstance(result, PerformanceTestResult)
            else
            # isinstance(result, ResultComparison)
            ReportFormatter.RESULT_COMPARISON_HEADER
        )

    @staticmethod
    def values(result):
        """Format values from PerformanceTestResult or ResultComparison.

        Returns tuple of strings to display in the results table.
        """
        return (
            (
                result.name,
                str(result.min_value) if result.min_value is not None else "-",
                str(result.max_value) if result.max_value is not None else "-",
                str(result.mean) if result.mean is not None else "-",
                str(result.max_rss) if result.max_rss is not None else "—",
            )
            if isinstance(result, PerformanceTestResult)
            else
            # isinstance(result, ResultComparison)
            (
                result.name,
                str(result.old.min_value) if result.old.min_value is not None else "-",
                str(result.new.min_value) if result.new.min_value is not None else "-",
                "{0:+.1f}%".format(result.delta),
                "{0:.2f}x{1}".format(result.ratio, " (?)" if result.is_dubious else ""),
            )
        )

    def markdown(self):
        """Report results of benchmark comparisons in Markdown format."""
        return self._formatted_text(
            label_formatter=lambda s: ("**" + s + "**"),
            COLUMN_SEPARATOR=" | ",
            DELIMITER_ROW=([":---"] + ["---:"] * 4),
            SEPARATOR="&nbsp; | | | | \n",
            SECTION="""
<details {3}>
  <summary>{0} ({1})</summary>
  {2}
</details>
""",
        )

    def git(self):
        """Report results of benchmark comparisons in 'git' format."""
        return self._formatted_text(
            label_formatter=lambda s: s.upper(),
            COLUMN_SEPARATOR="   ",
            DELIMITER_ROW=None,
            SEPARATOR="\n",
            SECTION="""
{0} ({1}): \n{2}""",
        )

    def _column_widths(self):
        changed = self.comparator.decreased + self.comparator.increased
        results = changed if self.changes_only else changed + self.comparator.unchanged
        results += self.comparator.added + self.comparator.removed

        widths = [
            map(len, columns)
            for columns in [
                ReportFormatter.PERFORMANCE_TEST_RESULT_HEADER,
                ReportFormatter.RESULT_COMPARISON_HEADER,
            ]
            + [ReportFormatter.values(r) for r in results]
        ]

        def max_widths(maximum, widths):
            return map(max, zip(maximum, widths))

        return list(functools.reduce(max_widths, widths, [0] * 5))

    def _formatted_text(
        self, label_formatter, COLUMN_SEPARATOR, DELIMITER_ROW, SEPARATOR, SECTION
    ):
        widths = self._column_widths()
        self.header_printed = False

        def justify_columns(contents):
            return [c.ljust(w) for w, c in zip(widths, contents)]

        def row(contents):
            return (
                ""
                if not contents
                else COLUMN_SEPARATOR.join(justify_columns(contents)) + "\n"
            )

        def header(title, column_labels):
            labels = (
                column_labels
                if not self.single_table
                else map(label_formatter, (title,) + column_labels[1:])
            )
            h = (
                ("" if not self.header_printed else SEPARATOR)
                + row(labels)
                + (row(DELIMITER_ROW) if not self.header_printed else "")
            )
            if self.single_table and not self.header_printed:
                self.header_printed = True
            return h

        def format_columns(r, is_strong):
            return r if not is_strong else r[:-1] + ("**" + r[-1] + "**",)

        def table(title, results, is_strong=False, is_open=False):
            if not results:
                return ""
            rows = [
                row(format_columns(ReportFormatter.values(r), is_strong))
                for r in results
            ]
            table = header(
                title if self.single_table else "",
                ReportFormatter.header_for(results[0]),
            ) + "".join(rows)
            return (
                table
                if self.single_table
                else SECTION.format(
                    title, len(results), table, "open" if is_open else ""
                )
            )

        return "\n" + "".join(
            [
                table("Regression", self.comparator.decreased, True, True),
                table("Improvement", self.comparator.increased, True),
                (
                    ""
                    if self.changes_only
                    else table("No Changes", self.comparator.unchanged)
                ),
                table("Added", self.comparator.added, is_open=True),
                table("Removed", self.comparator.removed, is_open=True),
            ]
        )

    HTML = """
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

    HTML_HEADER_ROW = """
        <tr>
                <th align='left'>{0} ({1})</th>
                <th align='left'>{2}</th>
                <th align='left'>{3}</th>
                <th align='left'>{4}</th>
                <th align='left'>{5}</th>
        </tr>
"""

    HTML_ROW = """
        <tr>
                <td align='left'>{0}</td>
                <td align='left'>{1}</td>
                <td align='left'>{2}</td>
                <td align='left'>{3}</td>
                <td align='left'><font color='{4}'>{5}</font></td>
        </tr>
"""

    def html(self):
        """Report results of benchmark comparisons in HTML format."""

        def row(name, old, new, delta, speedup, speedup_color):
            return self.HTML_ROW.format(name, old, new, delta, speedup_color, speedup)

        def header(contents):
            return self.HTML_HEADER_ROW.format(*contents)

        def table(title, results, speedup_color):
            rows = [
                row(*(ReportFormatter.values(r) + (speedup_color,))) for r in results
            ]
            return (
                ""
                if not rows
                else header(
                    (title, len(results)) + ReportFormatter.header_for(results[0])[1:]
                )
                + "".join(rows)
            )

        return self.HTML.format(
            "".join(
                [
                    table("Regression", self.comparator.decreased, "red"),
                    table("Improvement", self.comparator.increased, "green"),
                    (
                        ""
                        if self.changes_only
                        else table("No Changes", self.comparator.unchanged, "black")
                    ),
                    table("Added", self.comparator.added, ""),
                    table("Removed", self.comparator.removed, ""),
                ]
            )
        )


def parse_args(args):
    """Parse command line arguments and set default values."""
    parser = argparse.ArgumentParser(description="Compare Performance tests.")
    parser.add_argument(
        "--old-file", help="Baseline performance test suite (csv file)", required=True
    )
    parser.add_argument(
        "--new-file", help="New performance test suite (csv file)", required=True
    )
    parser.add_argument(
        "--format",
        choices=["markdown", "git", "html"],
        help="Output format. Default is markdown.",
        default="markdown",
    )
    parser.add_argument("--output", help="Output file name")
    parser.add_argument(
        "--changes-only", help="Output only affected tests", action="store_true"
    )
    parser.add_argument(
        "--single-table",
        help="Combine data in a single table in git and markdown formats",
        action="store_true",
    )
    parser.add_argument(
        "--delta-threshold",
        help="Delta threshold. Default 0.05.",
        type=float,
        default=0.05,
    )
    return parser.parse_args(args)


def create_report(
    old_results,
    new_results,
    delta_threshold,
    format,
    changes_only=True,
    single_table=True,
):
    comparator = TestComparator(old_results, new_results, delta_threshold)
    formatter = ReportFormatter(comparator, changes_only, single_table)
    formats = {
        "markdown": formatter.markdown,
        "git": formatter.git,
        "html": formatter.html,
    }

    report = formats[format]()
    return report


def main():
    """Compare benchmarks for changes in a formatted report."""
    args = parse_args(sys.argv[1:])
    report = create_report(
        LogParser.results_from_file(args.old_file),
        LogParser.results_from_file(args.new_file),
        args.delta_threshold,
        args.format,
        args.changes_only,
        args.single_table,
    )
    print(report)

    if args.output:
        with open(args.output, "w") as f:
            f.write(report)


if __name__ == "__main__":
    sys.exit(main())
