#!/usr/bin/python
# -*- coding: utf-8 -*-

# ===--- benchmark_utils.py ----------------------------------------------===//
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

import unittest

from compare_perf_tests import PerformanceTestResult
from compare_perf_tests import ResultComparison
from compare_perf_tests import break_word_camel_case


class TestPerformanceTestResult(unittest.TestCase):

    def test_init(self):
        log_line = '1,AngryPhonebook,20,10664,12933,11035,576,10884'
        r = PerformanceTestResult(log_line.split(','))
        self.assertEquals(r.name, 'AngryPhonebook')
        self.assertEquals((r.samples, r.min, r.max, r.mean, r.sd, r.median),
                          (20, 10664, 12933, 11035, 576, 10884))

        log_line = '1,AngryPhonebook,1,12045,12045,12045,0,12045,10510336'
        r = PerformanceTestResult(log_line.split(','))
        self.assertEquals(r.max_rss, 10510336)

    def test_header(self):
        self.assertEquals(PerformanceTestResult.header,
                          ('TEST', 'MIN', 'MAX', 'MEAN', 'MAX_RSS'))

    def test_values(self):
        log_line = '1,AngryPhonebook,20,10664,12933,11035,576,10884'
        r = PerformanceTestResult(log_line.split(','))
        self.assertEquals(
            r.values(break_words=True),
            (u'Angry\u200bPhonebook', '10664', '12933', '11035', '—')
        )
        self.assertEquals(
            r.values(),
            ('AngryPhonebook', '10664', '12933', '11035', '—')
        )
        log_line = '1,AngryPhonebook,1,12045,12045,12045,0,12045,10510336'
        r = PerformanceTestResult(log_line.split(','))
        self.assertEquals(r.values(break_words=True),
                          (u'Angry\u200bPhonebook',
                           '12045', '12045', '12045', '10510336'))

    def test_merge(self):
        tests = """1,AngryPhonebook,1,12045,12045,12045,0,12045,10510336
1,AngryPhonebook,1,12325,12325,12325,0,12325,10510336
1,AngryPhonebook,1,11616,11616,11616,0,11616,10502144
1,AngryPhonebook,1,12270,12270,12270,0,12270,10498048""".split('\n')
        results = map(PerformanceTestResult,
                      [line.split(',') for line in tests])

        def as_tuple(r):
            return (r.min, r.max, round(r.mean, 2), round(r.sd, 2), r.median,
                    r.max_rss)

        r = results[0]
        self.assertEquals(as_tuple(r),
                          (12045, 12045, 12045, 0, 12045, 10510336))
        r.merge(results[1])
        self.assertEquals(as_tuple(r),
                          (12045, 12325, 12185, 197.99, 12045, 10510336))
        r.merge(results[2])
        self.assertEquals(as_tuple(r),
                          (11616, 12325, 11995.33, 357.10, 12045, 10510336))
        r.merge(results[3])
        self.assertEquals(as_tuple(r),
                          (11616, 12325, 12064, 322.29, 12045, 10510336))


class TestBreakWordsCamelCase(unittest.TestCase):
    def test_break_words_camel_case(self):
        b = break_word_camel_case
        self.assertEquals(b('Unchanged'), 'Unchanged')
        self.assertEquals(b('AngryPhonebook'), u'Angry\u200bPhonebook')
        self.assertEquals(b('AngryPhonebook', separator='_'),
                          'Angry_Phonebook')
        self.assertEquals(b('ArrayAppendUTF16', separator='_'),
                          'Array_Append_UTF16')
        self.assertEquals(b('AnyHashableWithAClass', separator='_'),
                          'Any_Hashable_With_A_Class')
        self.assertEquals(b('ObjectiveCBridgeToNSArray', separator='_'),
                          'Objective_C_Bridge_To_NS_Array')
        self.assertEquals(b('SuffixAnySeqCRangeIterLazy', separator='_'),
                          'Suffix_Any_Seq_C_Range_Iter_Lazy')


class TestResultComparison(unittest.TestCase):
    def setUp(self):
        self.r0 = PerformanceTestResult(
            '101,GlobalClass,20,0,0,0,0,0,10185728'.split(','))
        self.r01 = PerformanceTestResult(
            '101,GlobalClass,20,20,20,20,0,0,10185728'.split(','))
        self.r1 = PerformanceTestResult(
            '1,AngryPhonebook,1,12325,12325,12325,0,12325,10510336'.split(','))
        self.r2 = PerformanceTestResult(
            '1,AngryPhonebook,1,11616,11616,11616,0,11616,10502144'.split(','))

    def test_init(self):
        rc = ResultComparison(self.r1, self.r2)
        self.assertEquals(rc.name, 'AngryPhonebook')
        self.assertAlmostEquals(rc.ratio, 12325.0 / 11616.0)
        self.assertAlmostEquals(rc.delta, (((11616.0 / 12325.0) - 1) * 100),
                                places=3)
        # handle test results that sometimes change to zero, when compiler
        # optimizes out the body of the incorrectly written test
        rc = ResultComparison(self.r0, self.r0)
        self.assertEquals(rc.name, 'GlobalClass')
        self.assertAlmostEquals(rc.ratio, 1)
        self.assertAlmostEquals(rc.delta, 0, places=3)
        rc = ResultComparison(self.r0, self.r01)
        self.assertAlmostEquals(rc.ratio, 0, places=3)
        self.assertAlmostEquals(rc.delta, 2000000, places=3)
        rc = ResultComparison(self.r01, self.r0)
        self.assertAlmostEquals(rc.ratio, 20001)
        self.assertAlmostEquals(rc.delta, -99.995, places=3)
        # disallow comparison of different test results
        self.assertRaises(
            AssertionError,
            ResultComparison, self.r0, self.r1
        )

    def test_header(self):
        self.assertEquals(ResultComparison.header,
                          ('TEST', 'OLD', 'NEW', 'DELTA', 'SPEEDUP'))

    def test_values(self):
        rc = ResultComparison(self.r1, self.r2)
        self.assertEquals(
            rc.values(break_words=True),
            (u'Angry\u200bPhonebook', '12325', '11616', '-5.8%', '1.06x')
        )
        self.assertEquals(
            rc.values(),
            ('AngryPhonebook', '12325', '11616', '-5.8%', '1.06x')
        )
        # other way around
        rc = ResultComparison(self.r2, self.r1)
        self.assertEquals(
            rc.values(),
            ('AngryPhonebook', '11616', '12325', '+6.1%', '0.94x')
        )

    def test_values_is_dubious(self):
        self.r2.max = self.r1.min + 1
        # new.min < old.min < new.max
        rc = ResultComparison(self.r1, self.r2)
        self.assertEquals(rc.values()[4], '1.06x (?)')
        # other way around: old.min < new.min < old.max
        rc = ResultComparison(self.r2, self.r1)
        self.assertEquals(rc.values()[4], '0.94x (?)')


if __name__ == '__main__':
    unittest.main()
