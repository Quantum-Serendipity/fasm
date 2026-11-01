// Copyright 2017-2022 F4PGA Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

/// Unit tests for ParseFasm

#include <gtest/gtest.h>
#include "ParseFasm.cpp"

// Test the Num class
TEST(ParseFasmTests, Num) {
        std::ostringstream str;
        str << Num('a', 0x74736554);
        if (str.str() == "aTest") {
                std::cout << "little endian" << std::endl;
        } else if (str.str() == "atseT") {
                std::cout << "big endian" << std::endl;
        } else {
                ASSERT_TRUE(false);
        }
}

// Check that count_without() works
TEST(ParseFasmTests, count_without) {
        std::string str = "_01_2_34_";
        EXPECT_EQ(count_without(str.begin(), str.end(), '_'), 5);
}

// clang-format off
// A test of the full parser
// Formatting is disabled to allow indenting the output string
// to illustrate the structure.
TEST(ParseFasmTests, parse_fasm) {
    std::istringstream input("a.b.c[31:0] = 7'o123 { d = \"e\", .f = \"\" } # hello\nthere");
    std::ostringstream output;
    bool stored_hex_mode = hex_mode;
    hex_mode = true;
    parse_fasm(input, output);
    
    /// The below string is the input above encoded in hex mode.
    EXPECT_EQ(output.str(),
        "l<50>" /// Line
          "s<20>" /// Set feature
            "f<5>a.b.c" /// Feature = "a.b.c"
            ":<7><1f><0>" /// Address = 31:0
            "'<7>" /// Width = 7
            "o<4><53>" /// Octal value = 123
          "{<1c>" /// Annotations (list)
            "a<a>" /// Annotation
              ".<1>d" /// Name = "d"
              "=<1>e" /// Value = "e"
            "a<a>" /// Annotation
              ".<2>.f" /// Name = ".f"
              "=<0>" /// Value = ""
          "#<6> hello" /// Comment
        "\n" /// Newline (only in hex mode for readability)
        "l<d>" /// Line
          "s<9>" /// Set feature
            "f<5>there" /// Feature = "there"
        "\n" /// Newline (only in hex mode for readability)
      );
     hex_mode = stored_hex_mode;
}
// clang-format on

static std::string parse_hex(const std::string& fasm) {
        std::istringstream input(fasm);
        std::ostringstream output;
        bool stored_hex_mode = hex_mode;
        hex_mode = true;
        try {
                parse_fasm(input, output);
        } catch (...) {
                hex_mode = stored_hex_mode;
                throw;
        }
        hex_mode = stored_hex_mode;
        return output.str();
}

TEST(ParseFasmTests, wide_decimal) {
        EXPECT_EQ(parse_hex("a[63:0] = 64'd4294967296"),
                  "l<24>s<1f>f<1>a:<7><3f><0>'<40>d<6><1><0>\n");
        EXPECT_EQ(parse_hex("a[63:0] = 64'd18446744073709551615"),
                  "l<33>s<2e>f<1>a:<7><3f><0>'<40>"
                  "d<14><ffffffff><ffffffff>\n");
        EXPECT_EQ(parse_hex("a[95:0] = 'd18_446_744_073_709_551_616"),
                  "l<22>s<1d>f<1>a:<7><5f><0>d<9><1><0><0>\n");
        EXPECT_EQ(parse_hex("a[31:0] = 'd0"),
                  "l<1c>s<17>f<1>a:<7><1f><0>d<3><0>\n");
}

TEST(ParseFasmTests, narrow_decimal_fields) {
        EXPECT_EQ(parse_hex("a[31:0] = 4294967295"),
                  "l<20>s<1b>f<1>a:<7><1f><0>p<ffffffff>\n");
        EXPECT_THROW(parse_hex("a[63:0] = 4294967296"), ParseException);
        EXPECT_THROW(parse_hex("a[4294967296] = 1"), ParseException);
        EXPECT_THROW(parse_hex("a[63:0] = 4294967296'd1"), ParseException);
}
