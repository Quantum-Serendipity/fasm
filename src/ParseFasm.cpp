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

#include "FasmLexer.h"
#include "FasmParser.h"
#include "FasmParserVisitor.h"
#include "antlr4-runtime.h"

/// This code parses FASM and produces a lightweight binary format that
/// is fast and simple to unpack based on the tag/length/value (TLV)
/// format.
///
/// For example, for a fixed width integer, this can just be:
///   <tag : 1 byte> <data : 4 bytes>
/// and for a variable length string:
///   <tag : 1 byte> <length : 4 bytes> <data : length bytes>
///
/// Note that the length itself must be a fixed width value, which does
/// impose a size limit, but this format is more efficient and easier
/// to decode than a UTF-8 style variable length encoding where a bit
/// is reserved per per byte to indicate the end of a variable length
/// value.
///
/// Each 4-bytes of a numeric value is in native endian order. This
/// format is designed to be produced and consumed on the same machine.
///
/// TLVs can be nested, with each level adding 5 bytes of header
/// overhead. There is a choice to aggregate values under another header
/// (using withHeader) or not. Even though this encodes redundant size
/// information, it can make the result easier to parse.
///
/// Example of a nested TLV:
///   <outer tag : 1 byte> <outer length = 5 + nA + 5 + nB : 4 bytes>
///     <A tag : 1 byte> <A length : 4 bytes> <A data : nA bytes>
///     <B tag : 1 byte> <B length : 4 bytes> <B data : nB bytes>
///
/// Note that there is no need for a closing tag.
///
/// For example, the consumer can allocate space for results in larger
/// chunks, in this case it can preallocate a line at a time, after
/// reading the first 5 bytes, although this is mostly useful for
/// consumers that can use the data directly without further
/// manipulation.
///
/// The format used here does not rely on knowing the size of the
/// entire output, which allows streaming line by line, although
/// ANTLR does not yet implement an incremental parser.
///
/// For a concrete example, see the test case in ParseFasmTests.cpp

/// External C Interface
///
/// These functions serialize the FASM parse tree to an easy to parse
/// tag/length/value binary format, where the tag is one byte and
/// the length is 4 bytes, in native endianness (typically little.)
extern "C" {
void from_string(const char* in,
                 bool hex,
                 void (*ret)(const char* str, size_t),
                 void (*err)(size_t, size_t, const char*));
void from_file(const char* path,
               bool hex,
               void (*ret)(const char* str, size_t),
               void (*err)(size_t, size_t, const char*));
}

using namespace antlr4;
using namespace antlrcpp;

/// Hex mode is useful for debugging.
/// In this mode, binary values are printed as hex values surrounded by < >
bool hex_mode = false;

/// The Num class provides an << operator that either dumps the raw value
/// or prints the hex value based on the mode. It also can print a tag.
class Num {
       public:
        /// Constructors, with optional tag character
        Num(char tag, uint32_t num) : num(num), tag(tag) {}
        Num(uint32_t num) : num(num), tag(0) {}

        uint32_t num;  /// The value
        char tag;      /// Tag character

        /// The bit width
        static constexpr int kWidth = sizeof(num) * 8;
};

/// Output stream operator for Num.
/// In hex mode, Nums are printed as <XX>, otherwise they are
/// copied into the output stream in the underlying representation.
/// As such, this will use the native endianness.
std::ostream& operator<<(std::ostream& s, const Num& num) {
        if (num.tag)
                s << num.tag;
        if (hex_mode) {
                s << "<" << std::hex << num.num << ">";
        } else {
                s.write(reinterpret_cast<const char*>(&num.num),
                        sizeof(num.num));
        }
        return s;
}

/// The Str class wraps a std::string to provide an << operator
/// that includes the tag and length.
struct Str {
        /// Takes the tag and string data
        Str(char tag, std::string data) : tag(tag), data(data) {}

        char tag;
        std::string data;
};

/// This output stream operator adds the header needed to decode
/// a string of unknown length and type.
/// Note that some characters are escaped in hex mode to
/// avoid confusion.
std::ostream& operator<<(std::ostream& s, const Str& str) {
        s << str.tag << Num(str.data.size());
        if (hex_mode) {  /// escape < \ >
                for (char c : str.data) {
                        if (c == '<' || c == '>' || c == '\\') {
                                s.put('\\');
                        }
                        s.put(c);
                }
        } else {
                s << str.data;
        }
        return s;
}

/// Wraps a string in another header, used to aggregate data.
std::string withHeader(char tag, std::string data) {
        std::ostringstream header;
        header << tag << Num(data.size());
        return header.str() + data;
}

bool is_digit_separator(char c) {
        return c == '_' || c == ' ' || c == '\t';
}

// clang-format off
int from_digit(char c) {
    return
        c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
        c >= 'A' && c <= 'F' ? c - 'A' + 10: -1;
}
// clang-format on

size_t bit_length(const std::vector<uint32_t>& words) {
        for (size_t i = words.size(); i > 0; i--) {
                uint32_t word = words[i - 1];
                if (word) {
                        size_t bits = (i - 1) * Num::kWidth;
                        while (word) {
                                bits++;
                                word >>= 1;
                        }
                        return bits;
                }
        }
        return 0;
}

/// Makes tags easy to extract for documentation and code generation.
/// Use at most once per line to allow simple grepping.
#define TAG(c, long_name) (c)

/// Raised on parse errors
struct ParseException {
        size_t line;          ///< Line number of error.
        size_t position;      ///< Position in that line.
        std::string message;  ///< A descriptive message.
};

ParseException parse_error(antlr4::Token* token, const std::string& message) {
        return ParseException{.line = token->getLine(),
                              .position = token->getCharPositionInLine(),
                              .message = message};
}

ParseException parse_error(antlr4::tree::TerminalNode* node,
                           const std::string& message) {
        return parse_error(node->getSymbol(), message);
}

std::vector<uint32_t> verilog_words(antlr4::tree::TerminalNode* node,
                                    unsigned radix,
                                    size_t bits_per_digit) {
        std::string text = node->getText();
        std::vector<uint32_t> words(1, 0);
        size_t digits = 0;
        for (auto it = text.begin() + 2; it != text.end(); it++) {
                if (is_digit_separator(*it)) {
                        continue;
                }
                int digit = from_digit(*it);
                if (digit < 0 || unsigned(digit) >= radix) {
                        throw parse_error(node, "Invalid digit in value.");
                }
                digits++;
                uint64_t carry = unsigned(digit);
                for (auto& word : words) {
                        uint64_t acc = uint64_t(word) * radix + carry;
                        word = uint32_t(acc);
                        carry = acc >> Num::kWidth;
                }
                if (carry) {
                        words.push_back(uint32_t(carry));
                }
        }
        if (digits == 0) {
                throw parse_error(node, "Value has no digits.");
        }
        size_t padded =
            (digits * bits_per_digit + Num::kWidth - 1) / Num::kWidth;
        while (words.size() < padded) {
                words.push_back(0);
        }
        return words;
}

uint32_t to_num(antlr4::tree::TerminalNode* node) {
        std::string text = node->getText();
        unsigned long long value = 0;
        size_t used = 0;
        try {
                value = std::stoull(text, &used);
        } catch (...) {
                used = 0;
        }
        if (used != text.size() ||
            value > std::numeric_limits<uint32_t>::max()) {
                throw ParseException{
                    .line = node->getSymbol()->getLine(),
                    .position = node->getSymbol()->getCharPositionInLine(),
                    .message = "Could not decode decimal number."};
        }
        return uint32_t(value);
}

/// Helper macro to convert a rule context into a string
/// For use inside FasmParserBaseVisitor
#define GET(x) (context->x() ? visit(context->x()).as<std::string>() : "")

/// FasmParserBaseVisitor is a visitor for the parse tree
/// generated by the ANTLR parser.
/// It will encode the tree a line at a time and stream out of
/// the given std::ostream
class FasmParserBaseVisitor : public FasmParserVisitor {
       public:
        static constexpr size_t kHeaderSize = 5;

        /// The constructor requires a std::ostream to stream encoded lines.
        /// This is to avoid storing an entire copy of the parse tree in a
        /// different form.
        FasmParserBaseVisitor(std::ostream& out) : out(out) {}

        /// Stream out FASM lines.
        virtual Any visitFasmFile(
            FasmParser::FasmFileContext* context) override {
                for (auto& line : context->fasmLine()) {
                        std::string str = visit(line).as<std::string>();
                        if (!str.empty()) {
                                out << str;
                                if (hex_mode)
                                        out << std::endl;
                        }
                }
                return {};
        }

        /// This is called for each FASM line.
        /// Tag: comment (#)
        /// Tag: line (l)
        virtual Any visitFasmLine(
            FasmParser::FasmLineContext* context) override {
                std::ostringstream data;
                data << GET(setFasmFeature) << GET(annotations);

                if (context->COMMENT_CAP()) {
                        std::string c = context->COMMENT_CAP()->getText();
                        c.erase(0, 1);  /// Remove the leading #
                        data << Str(TAG('#', comment), c);
                }

                if (!data.str().empty()) {
                        return withHeader(TAG('l', line), data.str());
                } else {
                        return std::string();  /// Don't emit empty lines.
                }
        }

        /// The set feature portion of a line (before annotations and comment.)
        /// Tag: feature (f)
        /// Tag: set feature (s)
        virtual Any visitSetFasmFeature(
            FasmParser::SetFasmFeatureContext* context) override {
                declared_width = -1;
                value_bits = 0;
                std::ostringstream data;
                data << Str(TAG('f', feature), context->FEATURE()->getText())
                     << GET(featureAddress) << GET(value);
                if (context->value()) {
                        check_value_width(context);
                }
                return withHeader(TAG('s', set_feature), data.str());
        }

        /// The bracketed address, where the second number is optional.
        /// Tag: address (:)
        virtual Any visitFeatureAddress(
            FasmParser::FeatureAddressContext* context) override {
                std::ostringstream data;
                data << Num(to_num(context->INT(0)));

                if (context->INT(1)) {
                        data << Num(to_num(context->INT(1)));
                }
                return withHeader(TAG(':', address), data.str());
        }

        /// A Verilog style number. It can be "plain" (no leading size and
        /// base), or hex (h), binary (b), decimal (d), or octal (o). Tag: bit
        /// width (')
        virtual Any visitVerilogValue(
            FasmParser::VerilogValueContext* context) override {
                std::ostringstream data;
                if (context->verilogDigits()) {
                        if (context->INT()) {
                                uint32_t width = to_num(context->INT());
                                declared_width = width;
                                data << Num(TAG('\'', width), width);
                        }
                        data << visit(context->verilogDigits())
                                    .as<std::string>();
                }
                return data.str();
        }

        /// A "plain" decimal value.
        /// Tag: plain (p)
        virtual Any visitPlainDecimal(
            FasmParser::PlainDecimalContext* context) override {
                std::ostringstream data;
                uint32_t value = to_num(context->INT());
                value_bits = bit_length({value});
                data << Num(TAG('p', plain), value);
                return data.str();
        }

        /// A Verilog hex value.
        /// Tag: hex (h)
        virtual Any visitHexValue(
            FasmParser::HexValueContext* context) override {
                return encode(
                    TAG('h', hex),
                    verilog_words(context->HEXADECIMAL_VALUE(), 16, 4));
        }

        /// A Verilog binary value.
        /// Tag: binary (b)
        virtual Any visitBinaryValue(
            FasmParser::BinaryValueContext* context) override {
                return encode(TAG('b', binary),
                              verilog_words(context->BINARY_VALUE(), 2, 1));
        }

        /// A Verilog decimal value.
        /// Tags: decimal (d)
        virtual Any visitDecimalValue(
            FasmParser::DecimalValueContext* context) override {
                return encode(TAG('d', decimal),
                              verilog_words(context->DECIMAL_VALUE(), 10, 0));
        }

        /// A Verilog octal value.
        /// Tags: octal (o)
        virtual Any visitOctalValue(
            FasmParser::OctalValueContext* context) override {
                return encode(TAG('o', octal),
                              verilog_words(context->OCTAL_VALUE(), 8, 3));
        }

        /// A collection of annotations. { ... }
        /// Tags: annotations ({)
        virtual Any visitAnnotations(
            FasmParser::AnnotationsContext* context) override {
                std::ostringstream data;
                for (auto& a : context->annotation()) {
                        data << visit(a).as<std::string>();
                }
                return withHeader(TAG('{', annotations), data.str());
        }

        /// An annotation: x = "y"
        /// Tags: annotation (a), annotation name (.), annotation value (=)
        virtual Any visitAnnotation(
            FasmParser::AnnotationContext* context) override {
                std::ostringstream data;
                data << Str(TAG('.', annotation_name),
                            context->ANNOTATION_NAME()->getText());
                if (context->ANNOTATION_VALUE()) {
                        std::string value =
                            context->ANNOTATION_VALUE()->getText();
                        value.erase(0, 1);  /// Convert "value" -> value
                        value.pop_back();
                        data << Str(TAG('=', annotation_value), value);
                }
                return withHeader(TAG('a', annotation), data.str());
        }

       private:
        std::string encode(char tag, const std::vector<uint32_t>& words) {
                value_bits = bit_length(words);
                std::ostringstream data;
                for (auto word = words.rbegin(); word != words.rend(); word++) {
                        data << Num(*word);
                }
                return withHeader(tag, data.str());
        }

        void check_value_width(FasmParser::SetFasmFeatureContext* context) {
                antlr4::Token* where = context->value()->getStart();
                int64_t address_width = 1;
                if (auto* address = context->featureAddress()) {
                        if (address->INT(1)) {
                                address_width =
                                    int64_t(to_num(address->INT(0))) -
                                    int64_t(to_num(address->INT(1))) + 1;
                        }
                }
                if (declared_width >= 0) {
                        if (int64_t(value_bits) > declared_width) {
                                throw parse_error(
                                    where,
                                    "Value is wider than its declared width "
                                    "of " +
                                        std::to_string(declared_width) +
                                        " bits.");
                        }
                        if (declared_width > address_width) {
                                throw parse_error(
                                    where,
                                    "Declared width of " +
                                        std::to_string(declared_width) +
                                        " bits exceeds the address width "
                                        "of " +
                                        std::to_string(address_width) +
                                        " bits.");
                        }
                }
                if (int64_t(value_bits) > std::max<int64_t>(address_width, 0)) {
                        throw parse_error(
                            where, "Value is wider than the address width of " +
                                       std::to_string(address_width) +
                                       " bits.");
                }
        }

        std::ostream& out;
        int64_t declared_width = -1;
        size_t value_bits = 0;
};

// Prevent use of the GET macro outside FasmParseBaseVisitor
#undef GET

class FasmErrorListener : public BaseErrorListener {
       public:
        virtual void syntaxError(Recognizer* recognizer,
                                 Token* token,
                                 size_t line,
                                 size_t position,
                                 const std::string& msg,
                                 std::exception_ptr e) override {
                throw ParseException{.line = line,
                                     .position = position,
                                     .message = std::string(msg)};
        }
};

/// Common portion of 'from_string' and 'from_file'.
/// Consumes an input stream and produces an output stream.
static void parse_fasm(std::istream& in, std::ostream& out) {
        ANTLRInputStream stream(in);
        FasmLexer lexer(&stream);
        FasmErrorListener errorListener;
        lexer.removeErrorListeners();
        lexer.addErrorListener(&errorListener);
        CommonTokenStream tokens(&lexer);
        FasmParser parser(&tokens);
        parser.removeErrorListeners();
        parser.addErrorListener(&errorListener);
        auto* tree = parser.fasmFile();
        FasmParserBaseVisitor(out).visit(tree);
}

/// Parse the given input string, returning output.
/// Use hex mode (see above) if hex is true.
/// Use a callback to avoid copying the result.
void from_string(const char* in,
                 bool hex,
                 void (*ret)(const char* str, size_t),
                 void (*err)(size_t, size_t, const char*)) {
        hex_mode = hex;
        std::istringstream input(in);
        std::ostringstream output;

        try {
                parse_fasm(input, output);
                output.put(0);
                std::string result = output.str();
                ret(result.c_str(), result.size());
        } catch (ParseException e) {
                // Parse failure will throw this exception.
                err(e.line, e.position, e.message.c_str());
        }
}

/// Parse the given input file, returning output.
/// Use hex mode (see above) if hex is true.
/// Use a callback to avoid copying the result.
void from_file(const char* path,
               bool hex,
               void (*ret)(const char* str, size_t),
               void (*err)(size_t, size_t, const char*)) {
        hex_mode = hex;
        std::fstream input(std::string(path), input.in);
        std::ostringstream output;
        if (input.is_open()) {
                try {
                        parse_fasm(input, output);
                        output.put(0);
                        std::string result = output.str();
                        ret(result.c_str(), result.size());
                } catch (ParseException e) {
                        // Parse failure will throw this exception.
                        err(e.line, e.position, e.message.c_str());
                }
        } else {
                err(0, 0, "Couldn't open file");
        }
}
