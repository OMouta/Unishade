#pragma once

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Reads the JSON the presets API answers with, and quotes text for the little JSON the app sends it. Text is UTF-8.
namespace json
{
struct Value
{
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    // A member of an object, or null when there is none, so a missing value reads as empty.
    const Value& operator[](std::string_view key) const
    {
        static const Value null;
        for (const auto& [name, value] : members)
            if (name == key)
                return value;
        return null;
    }

    bool IsNull() const { return type == Type::Null; }
    const std::string& String() const
    {
        static const std::string empty;
        return type == Type::String ? text : empty;
    }
    int64_t Integer() const { return type == Type::Number ? static_cast<int64_t>(number) : 0; }
    bool Bool() const { return type == Type::Bool && boolean; }
};

class Parser
{
public:
    explicit Parser(std::string_view text) : input(text) {}

    Value Document()
    {
        Value value = Parse(0);
        Space();
        if (position != input.size())
            Fail();
        return value;
    }

private:
    // Deeper nesting than the API sends means the answer is broken, and stops the recursion before the stack does.
    static constexpr int kMaxDepth = 32;

    [[noreturn]] void Fail() const { throw std::runtime_error("The server's answer is not valid JSON."); }

    void Space()
    {
        while (position < input.size() && (input[position] == ' ' || input[position] == '\t' || input[position] == '\n' || input[position] == '\r'))
            ++position;
    }

    bool Take(char c)
    {
        Space();
        if (position < input.size() && input[position] == c)
        {
            ++position;
            return true;
        }
        return false;
    }

    bool Word(std::string_view word)
    {
        if (input.substr(position, word.size()) != word)
            return false;
        position += word.size();
        return true;
    }

    unsigned Hex4()
    {
        if (position + 4 > input.size())
            Fail();
        unsigned value = 0;
        const auto [end, error] = std::from_chars(input.data() + position, input.data() + position + 4, value, 16);
        if (error != std::errc() || end != input.data() + position + 4)
            Fail();
        position += 4;
        return value;
    }

    static void AppendUtf8(std::string& text, unsigned code)
    {
        if (code < 0x80)
            text += static_cast<char>(code);
        else if (code < 0x800)
        {
            text += static_cast<char>(0xC0 | (code >> 6));
            text += static_cast<char>(0x80 | (code & 0x3F));
        }
        else if (code < 0x10000)
        {
            text += static_cast<char>(0xE0 | (code >> 12));
            text += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (code & 0x3F));
        }
        else
        {
            text += static_cast<char>(0xF0 | (code >> 18));
            text += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            text += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    std::string String()
    {
        if (!Take('"'))
            Fail();
        std::string text;
        for (;;)
        {
            if (position >= input.size())
                Fail();
            const char c = input[position++];
            if (c == '"')
                return text;
            if (static_cast<unsigned char>(c) < 0x20)
                Fail();
            if (c != '\\')
            {
                text += c;
                continue;
            }
            if (position >= input.size())
                Fail();
            switch (input[position++])
            {
            case '"': text += '"'; break;
            case '\\': text += '\\'; break;
            case '/': text += '/'; break;
            case 'b': text += '\b'; break;
            case 'f': text += '\f'; break;
            case 'n': text += '\n'; break;
            case 'r': text += '\r'; break;
            case 't': text += '\t'; break;
            case 'u':
            {
                unsigned code = Hex4();
                // A character beyond the first 65536 comes as two halves. A half on its own becomes U+FFFD.
                if (code >= 0xD800 && code < 0xDC00 && Word("\\u"))
                {
                    const unsigned low = Hex4();
                    code = low >= 0xDC00 && low < 0xE000 ? 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00) : 0xFFFD;
                }
                else if (code >= 0xD800 && code < 0xE000)
                    code = 0xFFFD;
                AppendUtf8(text, code);
                break;
            }
            default: Fail();
            }
        }
    }

    Value Parse(int depth)
    {
        if (depth > kMaxDepth)
            Fail();
        Space();
        if (position >= input.size())
            Fail();
        Value value;
        const char c = input[position];
        if (c == '"')
        {
            value.type = Value::Type::String;
            value.text = String();
        }
        else if (c == '{')
        {
            ++position;
            value.type = Value::Type::Object;
            if (Take('}'))
                return value;
            do
            {
                Space();
                std::string key = String();
                if (!Take(':'))
                    Fail();
                value.members.emplace_back(std::move(key), Parse(depth + 1));
            } while (Take(','));
            if (!Take('}'))
                Fail();
        }
        else if (c == '[')
        {
            ++position;
            value.type = Value::Type::Array;
            if (Take(']'))
                return value;
            do
                value.items.push_back(Parse(depth + 1));
            while (Take(','));
            if (!Take(']'))
                Fail();
        }
        else if (Word("true") || Word("false"))
        {
            value.type = Value::Type::Bool;
            value.boolean = c == 't';
        }
        else if (Word("null"))
            return value;
        else
        {
            value.type = Value::Type::Number;
            const auto [end, error] = std::from_chars(input.data() + position, input.data() + input.size(), value.number);
            if (error != std::errc())
                Fail();
            position = end - input.data();
        }
        return value;
    }

    std::string_view input;
    size_t position = 0;
};

// Throws std::runtime_error when the text is not JSON.
inline Value Parse(std::string_view text)
{
    return Parser(text).Document();
}

// text as a JSON string, quotes included.
inline std::string Quote(std::string_view text)
{
    constexpr char kHex[] = "0123456789abcdef";
    std::string quoted = "\"";
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
            quoted += '\\';
        if (byte < 0x20)
            quoted += std::string("\\u00") + kHex[byte >> 4] + kHex[byte & 15];
        else
            quoted += c;
    }
    return quoted + "\"";
}
} // namespace json
