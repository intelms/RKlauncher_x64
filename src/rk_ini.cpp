#include "rk_ini.h"
#include "rk_common.h"
#include <cstdio>
#include <cwchar>

namespace rk {

static bool IEquals(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring IniSection::GetString(const std::wstring& key, const std::wstring& defVal) const {
    for (const auto& e : entries) {
        if (IEquals(e.key, key)) return e.value;
    }
    return defVal;
}

int IniSection::GetInt(const std::wstring& key, int defVal) const {
    for (const auto& e : entries) {
        if (IEquals(e.key, key)) {
            if (e.value.empty()) return defVal;
            wchar_t* end = nullptr;
            long v = wcstol(e.value.c_str(), &end, 0);
            if (end != e.value.c_str()) return static_cast<int>(v);
            return defVal;
        }
    }
    return defVal;
}

bool IniSection::GetBool(const std::wstring& key, bool defVal) const {
    for (const auto& e : entries) {
        if (IEquals(e.key, key)) {
            if (_wcsicmp(e.value.c_str(), L"true") == 0 ||
                _wcsicmp(e.value.c_str(), L"yes") == 0 ||
                _wcsicmp(e.value.c_str(), L"1") == 0) {
                return true;
            }
            if (_wcsicmp(e.value.c_str(), L"false") == 0 ||
                _wcsicmp(e.value.c_str(), L"no") == 0 ||
                _wcsicmp(e.value.c_str(), L"0") == 0) {
                return false;
            }
            return GetInt(key, defVal ? 1 : 0) != 0;
        }
    }
    return defVal;
}

void IniSection::SetString(const std::wstring& key, const std::wstring& val) {
    for (auto& e : entries) {
        if (IEquals(e.key, key)) {
            e.value = val;
            return;
        }
    }
    entries.push_back({key, val});
}

void IniSection::SetInt(const std::wstring& key, int val) {
    SetString(key, std::to_wstring(val));
}

void IniSection::SetBool(const std::wstring& key, bool val) {
    SetString(key, val ? L"1" : L"0");
}

void IniFile::Clear() {
    sections_.clear();
}

bool IniFile::Load(const std::wstring& path) {
    Clear();
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return false;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp);
        return true;
    }

    std::vector<uint8_t> raw(static_cast<size_t>(sz));
    size_t got = fread(raw.data(), 1, raw.size(), fp);
    fclose(fp);
    raw.resize(got);

    std::wstring text;
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        encoding_ = Encoding::Utf16LE;
        size_t chars = (raw.size() - 2) / 2;
        text.resize(chars);
        memcpy(text.data(), raw.data() + 2, chars * 2);
    } else if (raw.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        encoding_ = Encoding::Utf16LE;
        size_t chars = (raw.size() - 2) / 2;
        text.resize(chars);
        for (size_t i = 0; i < chars; ++i) {
            text[i] = static_cast<wchar_t>((raw[2 + i * 2] << 8) | raw[3 + i * 2]);
        }
    } else if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        encoding_ = Encoding::Utf8OrAscii;
        std::string u8(reinterpret_cast<const char*>(raw.data() + 3), raw.size() - 3);
        text = Utf8ToWide(u8);
    } else {
        encoding_ = Encoding::Utf8OrAscii;
        std::string s(reinterpret_cast<const char*>(raw.data()), raw.size());
        text = Utf8ToWide(s);
    }

    IniSection* cur = nullptr;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = Trim(text.substr(pos, nl - pos));
        pos = nl + 1;

        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']' && line.size() >= 2) {
            cur = &AddSection(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) {
            if (!cur) cur = &AddSection(L"");
            std::wstring key = Trim(line.substr(0, eq));
            std::wstring val = Trim(line.substr(eq + 1));
            cur->entries.push_back({std::move(key), std::move(val)});
        }
    }
    return true;
}

bool IniFile::Save(const std::wstring& path, const std::wstring& headerComment) const {
    std::wstring content;
    if (!headerComment.empty()) {
        content += headerComment;
        if (content.back() != L'\n') content += L"\r\n";
    }
    for (size_t i = 0; i < sections_.size(); ++i) {
        const auto& sec = sections_[i];
        if (!sec.name.empty()) {
            if (i > 0 || !headerComment.empty()) content += L"\r\n";
            content += L"[" + sec.name + L"]\r\n";
        }
        for (const auto& e : sec.entries) {
            content += e.key + L" = " + e.value + L"\r\n";
        }
    }

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) return false;
    bool ok = true;
    if (encoding_ == Encoding::Utf16LE) {
        uint16_t bom = 0xFEFF;
        if (fwrite(&bom, 2, 1, fp) != 1) ok = false;
        if (ok && !content.empty()) {
            if (fwrite(content.data(), sizeof(wchar_t), content.size(), fp) != content.size()) ok = false;
        }
    } else {
        std::string u8 = WideToUtf8(content);
        if (!u8.empty()) {
            if (fwrite(u8.data(), 1, u8.size(), fp) != u8.size()) ok = false;
        }
    }
    fclose(fp);
    return ok;
}

const IniSection* IniFile::FindSection(const std::wstring& name) const {
    for (const auto& s : sections_) {
        if (IEquals(s.name, name)) return &s;
    }
    return nullptr;
}

IniSection& IniFile::GetOrAddSection(const std::wstring& name) {
    for (auto& s : sections_) {
        if (IEquals(s.name, name)) return s;
    }
    return AddSection(name);
}

IniSection& IniFile::AddSection(const std::wstring& name) {
    sections_.push_back({name, {}});
    return sections_.back();
}

std::wstring IniFile::GetString(const std::wstring& section, const std::wstring& key, const std::wstring& defVal) const {
    const IniSection* s = FindSection(section);
    return s ? s->GetString(key, defVal) : defVal;
}

int IniFile::GetInt(const std::wstring& section, const std::wstring& key, int defVal) const {
    const IniSection* s = FindSection(section);
    return s ? s->GetInt(key, defVal) : defVal;
}

bool IniFile::GetBool(const std::wstring& section, const std::wstring& key, bool defVal) const {
    const IniSection* s = FindSection(section);
    return s ? s->GetBool(key, defVal) : defVal;
}

void IniFile::SetString(const std::wstring& section, const std::wstring& key, const std::wstring& val) {
    GetOrAddSection(section).SetString(key, val);
}

void IniFile::SetInt(const std::wstring& section, const std::wstring& key, int val) {
    GetOrAddSection(section).SetInt(key, val);
}

void IniFile::SetBool(const std::wstring& section, const std::wstring& key, bool val) {
    GetOrAddSection(section).SetBool(key, val);
}

} // namespace rk
