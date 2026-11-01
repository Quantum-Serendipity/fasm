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

    def test_implementations(self):
        self.assertTrue('antlr' in fasm.parser.available)
        self.assertTrue('textx' in fasm.parser.available)


if __name__ == '__main__':
    unittest.main()
