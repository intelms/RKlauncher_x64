#pragma once

#include <string>
#include <utility>
#include <vector>

namespace rk {

struct IniEntry {
    std::wstring key;
    std::wstring value;
};

struct IniSection {
    std::wstring name;
    std::vector<IniEntry> entries;

    std::wstring GetString(const std::wstring& key, const std::wstring& defVal = L"") const;
    int GetInt(const std::wstring& key, int defVal = 0) const;
    bool GetBool(const std::wstring& key, bool defVal = false) const;
    void SetString(const std::wstring& key, const std::wstring& val);
    void SetInt(const std::wstring& key, int val);
    void SetBool(const std::wstring& key, bool val);
};

class IniFile {
public:
    enum class Encoding { Utf8OrAscii, Utf16LE };

    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path, const std::wstring& headerComment = L"") const;
    void Clear();

    const std::vector<IniSection>& Sections() const { return sections_; }
    std::vector<IniSection>& Sections() { return sections_; }

    const IniSection* FindSection(const std::wstring& name) const;
    IniSection& GetOrAddSection(const std::wstring& name);
    IniSection& AddSection(const std::wstring& name);

    std::wstring GetString(const std::wstring& section, const std::wstring& key, const std::wstring& defVal = L"") const;
    int GetInt(const std::wstring& section, const std::wstring& key, int defVal = 0) const;
    bool GetBool(const std::wstring& section, const std::wstring& key, bool defVal = false) const;

    void SetString(const std::wstring& section, const std::wstring& key, const std::wstring& val);
    void SetInt(const std::wstring& section, const std::wstring& key, int val);
    void SetBool(const std::wstring& section, const std::wstring& key, bool val);

    void SetEncoding(Encoding enc) { encoding_ = enc; }

private:
    std::vector<IniSection> sections_;
    Encoding encoding_ = Encoding::Utf8OrAscii;
};

} // namespace rk
