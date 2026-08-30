#include "PathUtil.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace npsync
{

namespace
{

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::wstring wlower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    std::replace(s.begin(), s.end(), L'/', L'\\');
    return s;
}

} // namespace

bool PathUtil::normalizeRelative(const std::string& in, std::string& out) {
    out.clear();
    if (in.empty() || in.size() > 4096)
        return false;

    std::string s = in;
    std::replace(s.begin(), s.end(), '\\', '/');

    // Reject absolute paths, drive letters, UNC, and NULs up front.
    if (s[0] == '/' || (s.size() > 1 && s[1] == ':'))
        return false;
    if (s.find('\0') != std::string::npos)
        return false;

    // Resolve segments; ".." may never pop above the root.
    std::vector<std::string> segs;
    std::istringstream ss(s);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty() || seg == ".")
            continue;
        if (seg == "..") {
            if (segs.empty())
                return false; // escapes the root
            segs.pop_back();
            continue;
        }
        segs.push_back(seg);
    }
    if (segs.empty())
        return false;

    for (size_t i = 0; i < segs.size(); ++i) {
        if (i)
            out.push_back('/');
        out += segs[i];
    }
    if (hasIllegalChars(out) || hasReservedName(out))
        return false;
    return true;
}

bool PathUtil::hasIllegalChars(const std::string& relPath) {
    static const std::string illegal = "<>:\"|?*";
    for (unsigned char c : relPath) {
        if (c < 0x20)
            return true;
        if (illegal.find(static_cast<char>(c)) != std::string::npos)
            return true;
    }
    // Trailing dots/spaces are not allowed in Windows filenames.
    size_t slash = relPath.find_last_of('/');
    std::string base = slash == std::string::npos ? relPath : relPath.substr(slash + 1);
    if (!base.empty() && (base.back() == '.' || base.back() == ' '))
        return true;
    return false;
}

bool PathUtil::hasReservedName(const std::string& relPath) {
    static const char* names[] = {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
                                  "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                  "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    std::istringstream ss(relPath);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        std::string base = lower(seg);
        size_t dot = base.find('.');
        if (dot != std::string::npos)
            base = base.substr(0, dot);
        for (const char* n : names)
            if (base == n)
                return true;
    }
    return false;
}

bool PathUtil::isInsideRoot(const std::wstring& absRoot, const std::wstring& absPath) {
    std::wstring root = wlower(absRoot);
    std::wstring path = wlower(absPath);
    while (!root.empty() && root.back() == L'\\')
        root.pop_back();
    if (path == root)
        return true;
    if (path.size() <= root.size())
        return false;
    return path.compare(0, root.size(), root) == 0 && path[root.size()] == L'\\';
}

bool PathUtil::joinInsideRoot(const std::wstring& absRoot, const std::string& relNormalized,
                              std::wstring& absOut) {
    // Defense in depth: normalize again even though callers should pass
    // pre-normalized paths.
    std::string rel;
    if (!normalizeRelative(relNormalized, rel))
        return false;
    std::wstring wrel = utf8ToWide(rel);
    std::replace(wrel.begin(), wrel.end(), L'/', L'\\');
    std::wstring root = absRoot;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();
    std::wstring full = root + L"\\" + wrel;
    if (!isInsideRoot(root, full))
        return false;
    absOut = std::move(full);
    return true;
}

std::wstring PathUtil::utf8ToWide(const std::string& s) {
    if (s.empty())
        return {};
    int len =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string PathUtil::wideToUtf8(const std::wstring& s) {
    if (s.empty())
        return {};
    int len =
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace npsync
