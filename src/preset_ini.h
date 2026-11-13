#pragma once

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A ReShade preset: keys before the first section, such as Techniques, then a section per effect file. Keeps
// the order of sections and keys, so saving a preset changes only what changed. Comments, blank lines and other
// lines that are not keys stay where they were, and so do sections without keys. Section and key names compare
// without case, since effect files are named without case on Windows. Shared by every platform's host and
// Setup.
class PresetIni
{
public:
    struct Section
    {
        std::string name; // empty for the keys before the first section
        std::vector<std::pair<std::string, std::string>> values;
        // Lines that are not keys, such as comments, each with the number of keys before it in the section.
        std::vector<std::pair<size_t, std::string>> otherLines;
        bool fromText = false; // a section the text had, which is written even without keys
    };

    PresetIni() { sections.push_back({}); }

    explicit PresetIni(const std::string& text) : PresetIni()
    {
        std::istringstream stream(text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? text.substr(3) : text);
        Section* current = &sections.front();
        for (std::string line; std::getline(stream, line);)
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            const size_t first = line.find_first_not_of(" \t");
            if (first != std::string::npos && line[first] != ';' && line[first] != '#')
            {
                const std::string_view content = std::string_view(line).substr(first);
                if (content.front() == '[' && content.back() == ']')
                {
                    const std::string name(content.substr(1, content.size() - 2));
                    current = FindSection(name);
                    if (!current)
                    {
                        sections.push_back({ name, {}, {}, true });
                        current = &sections.back();
                    }
                    current->fromText = true;
                    continue;
                }
                if (const size_t equals = content.find('='); equals != std::string_view::npos)
                {
                    std::string key(content.substr(0, equals));
                    while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
                        key.pop_back();
                    std::string value(content.substr(equals + 1));
                    value.erase(0, value.find_first_not_of(" \t"));
                    const auto known = std::find_if(current->values.begin(), current->values.end(),
                                                    [&key](const auto& entry) { return Same(entry.first, key); });
                    if (known != current->values.end())
                        known->second = std::move(value);
                    else
                        current->values.emplace_back(std::move(key), std::move(value));
                    continue;
                }
            }
            current->otherLines.emplace_back(current->values.size(), std::move(line));
        }
    }

    bool Get(const std::string& section, const std::string& key, std::string& value) const
    {
        if (const Section* found = FindSection(section))
            for (const auto& [name, text] : found->values)
                if (Same(name, key))
                {
                    value = text;
                    return true;
                }
        return false;
    }

    bool HasSection(const std::string& section) const { return FindSection(section) != nullptr; }

    std::vector<std::string> SectionNames() const
    {
        std::vector<std::string> names;
        for (const Section& section : sections)
            if (!section.name.empty())
                names.push_back(section.name);
        return names;
    }

    void Set(const std::string& section, const std::string& key, const std::string& value)
    {
        Section* found = FindSection(section);
        if (!found)
        {
            sections.push_back({ section, {}, {}, false });
            found = &sections.back();
        }
        SetIn(*found, key, value);
    }

    void Remove(const std::string& section, const std::string& key)
    {
        Section* found = FindSection(section);
        if (!found)
            return;
        for (size_t index = found->values.size(); index-- > 0;)
            if (Same(found->values[index].first, key))
            {
                found->values.erase(found->values.begin() + static_cast<std::ptrdiff_t>(index));
                // The lines after it move up with the keys.
                for (auto& other : found->otherLines)
                    if (other.first > index)
                        --other.first;
            }
    }

    std::string Text() const
    {
        std::string text;
        for (const Section& section : sections)
        {
            if (!section.name.empty())
            {
                if (section.values.empty() && section.otherLines.empty() && !section.fromText)
                    continue;
                // A blank line before a new section, unless the preset ends with one. Sections the text had stay
                // as they were.
                if (!section.fromText && !text.empty() && !text.ends_with("\n\n"))
                    text += "\n";
                text += "[" + section.name + "]\n";
            }
            size_t other = 0;
            for (size_t index = 0; index <= section.values.size(); ++index)
            {
                for (; other < section.otherLines.size() && section.otherLines[other].first <= index; ++other)
                    text += section.otherLines[other].second + "\n";
                if (index < section.values.size())
                    text += section.values[index].first + "=" + section.values[index].second + "\n";
            }
        }
        return text;
    }

    // "a,b,c" as ReShade writes lists.
    static std::vector<std::string> Split(const std::string& text)
    {
        std::vector<std::string> items;
        for (size_t start = 0; start < text.size();)
        {
            const size_t end = std::min(text.find(',', start), text.size());
            if (end > start)
                items.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        return items;
    }

    // Names compared as presets compare them: ASCII letters without case.
    static bool Same(const std::string& a, const std::string& b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
    }

    static std::string Join(const std::vector<std::string>& items)
    {
        std::string text;
        for (const std::string& item : items)
            text += (text.empty() ? "" : ",") + item;
        return text;
    }

private:
    Section* FindSection(const std::string& name)
    {
        for (Section& section : sections)
            if (Same(section.name, name))
                return &section;
        return nullptr;
    }
    const Section* FindSection(const std::string& name) const { return const_cast<PresetIni*>(this)->FindSection(name); }

    static void SetIn(Section& section, const std::string& key, const std::string& value)
    {
        for (auto& entry : section.values)
            if (Same(entry.first, key))
            {
                entry.second = value;
                return;
            }
        // After the last key, before the blank lines and comments that end the section.
        for (auto& other : section.otherLines)
            if (other.first == section.values.size())
                ++other.first;
        section.values.emplace_back(key, value);
    }

    static char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

    std::vector<Section> sections;
};

// The effect files whose techniques the preset turns on, in order and each once. Techniques are listed as
// Name@File.fx; entries without a file are left out.
inline std::vector<std::string> PresetEffectFiles(const PresetIni& preset)
{
    std::vector<std::string> files;
    std::string techniques;
    if (!preset.Get("", "Techniques", techniques))
        return files;
    for (const std::string& technique : PresetIni::Split(techniques))
    {
        const size_t at = technique.find('@');
        std::string file = at == std::string::npos ? std::string() : technique.substr(at + 1);
        file.erase(file.find_last_not_of(" \t") + 1);
        if (!file.empty() && std::none_of(files.begin(), files.end(), [&file](const std::string& known) { return PresetIni::Same(known, file); }))
            files.push_back(std::move(file));
    }
    return files;
}
