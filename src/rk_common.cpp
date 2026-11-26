#include "rk_common.h"
#include <shlwapi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>

namespace rk {

bool g_debugLog = false;

std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L".";
    PathRemoveFileSpecW(buf);
    return buf;
}

std::wstring NormalizeSlashes(std::wstring s, wchar_t sep) {
    wchar_t other = (sep == L'/') ? L'\\' : L'/';
    std::replace(s.begin(), s.end(), other, sep);
    return s;
}

std::wstring ResolveAppPath(const std::wstring& relOrAbs) {
    if (relOrAbs.empty()) return L"";
    std::wstring s = NormalizeSlashes(relOrAbs, L'\\');
    if (!PathIsRelativeW(s.c_str())) {
        return s;
    }
    std::wstring base = GetExeDir();
    if (!base.empty() && base.back() != L'\\') base.push_back(L'\\');
    wchar_t joined[MAX_PATH] = {};
    if (PathCombineW(joined, base.c_str(), s.c_str())) {
        return joined;
    }
    return base + s;
}

std::wstring MakeRelativeIfInsideAppDir(const std::wstring& path) {
    if (path.empty()) return L"";
    std::wstring dir = ToLower(NormalizeSlashes(GetExeDir(), L'\\'));
    if (!dir.empty() && dir.back() != L'\\') dir.push_back(L'\\');
    std::wstring norm = NormalizeSlashes(path, L'\\');
    std::wstring low = ToLower(norm);
    if (low.size() > dir.size() && low.compare(0, dir.size(), dir) == 0) {
        return NormalizeSlashes(norm.substr(dir.size()), L'/');
    }
    return path;
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirExists(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ToLower(std::wstring s) {
    for (wchar_t& ch : s) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), nullptr, 0);
    UINT cp = CP_UTF8;
    if (needed <= 0) {
        cp = CP_ACP;
        needed = MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), nullptr, 0);
    }
    if (needed <= 0) return {};
    std::wstring out(needed, L'\0');
    MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), out.data(), needed);
    return out;
}

bool IsPeMachineX64(const std::wstring& dllPath, uint16_t* outMachine) {
    if (outMachine) *outMachine = 0;
    HANDLE hFile = CreateFileW(dllPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    IMAGE_DOS_HEADER dos = {};
    DWORD read = 0;
    bool ok = ReadFile(hFile, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) &&
              dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= 0 && dos.e_lfanew < 0x100000;
    if (ok) {
        LARGE_INTEGER pos{};
        pos.QuadPart = dos.e_lfanew;
        if (SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN)) {
            DWORD sig = 0;
            IMAGE_FILE_HEADER fh = {};
            if (ReadFile(hFile, &sig, sizeof(sig), &read, nullptr) && read == sizeof(sig) &&
                sig == IMAGE_NT_SIGNATURE &&
                ReadFile(hFile, &fh, sizeof(fh), &read, nullptr) && read == sizeof(fh)) {
                if (outMachine) *outMachine = fh.Machine;
                CloseHandle(hFile);
                return fh.Machine == IMAGE_FILE_MACHINE_AMD64;
            }
        }
    }
    CloseHandle(hFile);
    return false;
}

void DebugLog(const wchar_t* fmt, ...) {
    if (!g_debugLog) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    std::wstring logPath = ResolveAppPath(L"rklauncher_x64.log");
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, logPath.c_str(), L"a, ccs=UTF-8") == 0 && fp) {
        fwprintf(fp, L"%s\n", buf);
        fclose(fp);
    }
}

} // namespace rk
