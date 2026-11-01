#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright 2017-2022 F4PGA Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

import os
import os.path
import importlib

import unittest
import fasm
import fasm.parser

parsers = {}
for name in fasm.parser.available:
    parsers[name] = importlib.import_module('fasm.parser.' + name)


def example(fname):
    return os.path.join(os.path.dirname(__file__), '..', 'examples', fname)


def check_round_trip(test, parser, result):
    s = fasm.fasm_tuple_to_string(result)
    test.assertEqual(list(parser.parse_fasm_string(s)), result)


class TestFasm(unittest.TestCase):
    def test_blank_file(self):
        for name, parser in parsers.items():
            with self.subTest(name, parser=name):
                result = list(
                    parser.parse_fasm_filename(example('blank.fasm')))
                self.assertEqual(result, [])

                check_round_trip(self, parser, result)

    def test_comment_file(self):
        for name, parser in parsers.items():
            with self.subTest(name, parser=name):
                result = list(
                    parser.parse_fasm_filename(example('comment.fasm')))
                self.assertEqual(
                    result, [
                        fasm.FasmLine(
                            set_feature=None,
                            annotations=None,
                            comment=' Only a comment.',
                        )
                    ])

                check_round_trip(self, parser, result)

    def test_one_line_feature(self):
        for name, parser in parsers.items():
            with self.subTest(name, parser=name):
                result = list(
                    parser.parse_fasm_filename(example('feature_only.fasm')))
                self.assertEqual(
                    result, [
                        fasm.FasmLine(
                            set_feature=fasm.SetFasmFeature(
                                feature='EXAMPLE_FEATURE.X0.Y0.BLAH',
                                start=None,
                                end=None,
                                value=1,
                                value_format=None,
                            ),
                            annotations=None,
                            comment=None,
                        )
                    ])

                self.assertEqual(
                    fasm.fasm_tuple_to_string(result),
                    'EXAMPLE_FEATURE.X0.Y0.BLAH\n')
                check_round_trip(self, parser, result)

    def test_examples_file(self):
        for name, parser in parsers.items():
            with self.subTest(name, parser=name):
                result = list(parser.parse_fasm_filename(example('many.fasm')))
                check_round_trip(self, parser, result)

    def test_wide_decimal_values(self):
        cases = [
            ('A.X[63:0] = 64\'d4294967296', 4294967296),
            ('A.X[63:0] = 64\'d18446744073709551615', 2**64 - 1),
            ('A.X[95:0] = \'d18_446_744_073_709_551_616', 2**64),
            ('A.X[31:0] = 4294967295', 2**32 - 1),
        ]
        for name, parser in parsers.items():
            for text, value in cases:
                with self.subTest(name, parser=name, text=text):
                    result = list(parser.parse_fasm_string(text))
                    self.assertEqual(result[0].set_feature.value, value)
                    check_round_trip(self, parser, result)
        for text, _ in cases:
            with self.subTest(text=text):
                self.assertEqual(
                    list(parsers['antlr'].parse_fasm_string(text)),
                    list(parsers['textx'].parse_fasm_string(text)))

    def test_plain_decimal_above_one_word_refused(self):
        with self.assertRaises(Exception):
            parsers['antlr'].parse_fasm_string('A.X[63:0] = 4294967296')

    def test_value_digit_separators(self):
        cases = [
            ('A.X[127:0] = \'d 5', 5),
            ('A.X[127:0] = 128\'d 5', 5),
            ('A.X[127:0] = \'d\t1_0', 10),
            ('A.X[63:0] = 64\'h 5', 5),
            ('A.X[127:0] = \'h\t5', 5),
            ('A.X[127:0] = \'b 101', 5),
            ('A.X[127:0] = \'o 17', 15),
            ('A.X[127:0] = \'o\t17', 15),
            ('A.X[63:0] = \'o777777777777', 2**36 - 1),
            ('A.X[63:0] = \'o1234567012345670', 0o1234567012345670),
            ('A.X[7:0] = 8\'d255', 255),
            ('A.X[0:7] = 0', 0),
        ]
        for text, value in cases:
            with self.subTest(text=text):
                got = {
                    name: list(parser.parse_fasm_string(text))
                    for name, parser in parsers.items()
                }
                self.assertEqual(got['antlr'], got['textx'])
                self.assertEqual(got['antlr'][0].set_feature.value, value)

    def test_values_refused_by_both(self):
        cases = [
            'A.X[7:0] = \'d_',
            'A.X[7:0] = \'h_',
            'A.X[7:0] = \'b_',
            'A.X[7:0] = \'o _',
            'A.X[7:0] = 16\'d1',
            'A = 2\'d1',
            'A.X[0:7] = 1\'d0',
            'A.X[7:0] = 2\'d6',
            'A.X[3:0] = \'d16',
            'A.X[3:0] = 16',
        ]
        for text in cases:
            for name, parser in parsers.items():
                with self.subTest(name, parser=name, text=text):
                    with self.assertRaises(Exception):
                        list(parser.parse_fasm_string(text))

    def test_oversize_value_message(self):
        with self.assertRaisesRegex(
                Exception, 'wider than its declared width of 2 bits'):
            parsers['antlr'].parse_fasm_string('A.X[7:0] = 2\'d6')
        with self.assertRaisesRegex(
                Exception, 'wider than the address width of 4 bits'):
            parsers['antlr'].parse_fasm_string('A.X[3:0] = \'d16')
        with self.assertRaisesRegex(
                Exception, 'exceeds the address width of 8 bits'):
            parsers['antlr'].parse_fasm_string('A.X[7:0] = 16\'d1')

    def test_implementations(self):
        self.assertTrue('antlr' in fasm.parser.available)
        self.assertTrue('textx' in fasm.parser.available)


if __name__ == '__main__':
    unittest.main()
