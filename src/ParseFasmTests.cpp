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

TEST(ParseFasmTests, bit_length) {
        EXPECT_EQ(bit_length({0}), 0u);
        EXPECT_EQ(bit_length({1}), 1u);
        EXPECT_EQ(bit_length({0xffffffff}), 32u);
        EXPECT_EQ(bit_length({0, 1}), 33u);
        EXPECT_EQ(bit_length({5, 0, 0}), 3u);
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

static std::string parse_error_message(const std::string& fasm) {
        try {
                parse_hex(fasm);
        } catch (const ParseException& e) {
                return e.message;
        }
        return "";
}

TEST(ParseFasmTests, value_digit_separators) {
        EXPECT_EQ(parse_hex("a[127:0] = 'd 5"),
                  "l<1c>s<17>f<1>a:<7><7f><0>d<3><5>\n");
        EXPECT_EQ(parse_hex("a[127:0] = 128'd 5"),
                  parse_hex("a[127:0] = 128'd5"));
        EXPECT_EQ(parse_hex("a[127:0] = 'd\t1_0"),
                  parse_hex("a[127:0] = 'd10"));
        EXPECT_EQ(parse_hex("a[63:0] = 64'h 5"), parse_hex("a[63:0] = 64'h5"));
        EXPECT_EQ(parse_hex("a[127:0] = 'h\t5"), parse_hex("a[127:0] = 'h5"));
        EXPECT_EQ(parse_hex("a[127:0] = 'b 101"),
                  parse_hex("a[127:0] = 'b101"));
        EXPECT_EQ(parse_hex("a[127:0] = 'b\t1"), parse_hex("a[127:0] = 'b1"));
        EXPECT_EQ(parse_hex("a[127:0] = 'o 17"), parse_hex("a[127:0] = 'o17"));
        EXPECT_EQ(parse_hex("a[127:0] = 'o\t17"), parse_hex("a[127:0] = 'o17"));
}

TEST(ParseFasmTests, multi_word_octal) {
        EXPECT_EQ(parse_hex("a[63:0] = 'o777777777777"),
                  "l<26>s<21>f<1>a:<7><3f><0>o<d><f><ffffffff>\n");
}

TEST(ParseFasmTests, value_without_digits) {
        EXPECT_THROW(parse_hex("a[7:0] = 'd_"), ParseException);
        EXPECT_THROW(parse_hex("a[7:0] = 'h_"), ParseException);
        EXPECT_THROW(parse_hex("a[7:0] = 'b_"), ParseException);
        EXPECT_THROW(parse_hex("a[7:0] = 'o _"), ParseException);
        EXPECT_EQ(parse_error_message("a[7:0] = 'd_"), "Value has no digits.");
}

TEST(ParseFasmTests, value_widths) {
        EXPECT_EQ(parse_hex("a[7:0] = 8'd255"),
                  "l<20>s<1b>f<1>a:<6><7><0>'<8>d<4><ff>\n");
        EXPECT_EQ(parse_hex("a[0:7] = 0"), "l<18>s<13>f<1>a:<6><0><7>p<0>\n");
        EXPECT_EQ(parse_error_message("a[7:0] = 16'd1"),
                  "Declared width of 16 bits exceeds the address width of 8 "
                  "bits.");
        EXPECT_THROW(parse_hex("a = 2'd1"), ParseException);
        EXPECT_THROW(parse_hex("a[0:7] = 1'd0"), ParseException);
        EXPECT_EQ(parse_error_message("a[7:0] = 2'd6"),
                  "Value is wider than its declared width of 2 bits.");
        EXPECT_EQ(parse_error_message("a[3:0] = 'd16"),
                  "Value is wider than the address width of 4 bits.");
        EXPECT_THROW(parse_hex("a[3:0] = 16"), ParseException);
        EXPECT_THROW(parse_hex("a[0:7] = 1"), ParseException);
}
