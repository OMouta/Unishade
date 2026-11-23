#pragma once

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Edits an INI file as text, keeping its byte order mark, line endings and everything it does not change.
// ReShade.ini is UTF-8, which GetPrivateProfileString would read in the ANSI code page. Like the Windows profile
// functions, it ignores spaces and tabs around section names, keys and values, and the case of section names and
// keys, so hand-edited files read as their author meant.
class IniText
{
public:
    explicit IniText(std::string text)
    {
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        {
            bom = text.substr(0, 3);
            text.erase(0, 3);
        }
        newline = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
        std::istringstream stream(text);
        for (std::string line; std::getline(stream, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
        }
    }

    // Returns false when the key is missing.
    bool Get(const std::string& section, const std::string& key, std::string& value) const
    {
        const size_t line = Find(section, key);
        if (line == std::string::npos)
            return false;
        value = Trim(std::string_view(lines[line]).substr(lines[line].find('=') + 1));
        return true;
    }

    void Set(const std::string& section, const std::string& key, const std::string& value)
    {
        changed = true;
        if (const size_t line = Find(section, key); line != std::string::npos)
        {
            // The key keeps how it was written, with the spaces around the equals sign.
            const size_t equals = lines[line].find('=');
            const size_t start = lines[line].find_first_not_of(" \t", equals + 1);
            lines[line] = lines[line].substr(0, start == std::string::npos ? lines[line].size() : start) + value;
            return;
        }
        size_t line = 0;
        while (line < lines.size() && !IsHeader(lines[line], section))
            ++line;
        if (line == lines.size())
        {
            if (!lines.empty() && !Trim(lines.back()).empty())
                lines.push_back("");
            line = lines.size();
            lines.push_back("[" + section + "]");
        }
        // After the section's last entry, before the blank line that ends it.
        size_t end = line + 1;
        while (end < lines.size() && !Trim(lines[end]).empty() && Trim(lines[end])[0] != '[')
            ++end;
        lines.insert(lines.begin() + end, key + "=" + value);
    }

    bool Changed() const { return changed; }

    std::string Text() const
    {
        std::string text = bom;
        for (const auto& line : lines)
            text += line + newline;
        return text;
    }

private:
    static std::string_view Trim(std::string_view text)
    {
        const size_t start = text.find_first_not_of(" \t");
        if (start == std::string_view::npos)
            return {};
        return text.substr(start, text.find_last_not_of(" \t") - start + 1);
    }

    // ASCII only, as section names and keys are.
    static bool SameName(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const char x = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] - 'A' + 'a') : a[i];
            const char y = b[i] >= 'A' && b[i] <= 'Z' ? char(b[i] - 'A' + 'a') : b[i];
            if (x != y)
                return false;
        }
        return true;
    }

    // Whether the line is the header of the section. Text after the closing bracket, such as a comment, is ignored.
    static bool IsHeader(std::string_view line, std::string_view section)
    {
        line = Trim(line);
        const size_t close = line.find(']');
        return !line.empty() && line[0] == '[' && close != std::string_view::npos && SameName(Trim(line.substr(1, close - 1)), section);
    }

    size_t Find(const std::string& section, const std::string& key) const
    {
        bool inside = false;
        for (size_t line = 0; line < lines.size(); ++line)
        {
            const std::string_view text = Trim(lines[line]);
            if (!text.empty() && text[0] == '[')
                inside = IsHeader(text, section);
            else if (const size_t equals = text.find('='); inside && equals != std::string_view::npos && SameName(Trim(text.substr(0, equals)), key))
                return line;
        }
        return std::string::npos;
    }

    std::vector<std::string> lines;
    std::string bom;
    std::string newline;
    bool changed = false;
};
