// PathUtil.h — path normalization and containment validation.
//
// Security-critical: relative paths arrive from *other clients* via encrypted
// metadata. A malicious or buggy peer must never cause a write outside the
// configured sync roots (path traversal defense).
#pragma once

#include <string>

namespace npsync
{

class PathUtil {
  public:
    // Normalize a sync-relative path: forward slashes, collapsed separators,
    // resolved "." / ".." (clamped at root), no drive letters or leading
    // slashes. Returns false if the path is fundamentally invalid.
    static bool normalizeRelative(const std::string& in, std::string& out);

    // True iff `absPath` lies inside `absRoot` (both normalized absolute
    // paths, case-insensitive Windows semantics).
    static bool isInsideRoot(const std::wstring& absRoot, const std::wstring& absPath);

    // Join an absolute root with a normalized relative path, producing an
    // absolute wide path guaranteed to stay inside the root. Returns false
    // on any traversal attempt.
    static bool joinInsideRoot(const std::wstring& absRoot, const std::string& relNormalized,
                               std::wstring& absOut);

    // Characters forbidden in Windows filenames + control chars.
    static bool hasIllegalChars(const std::string& relPath);

    // Windows reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9).
    static bool hasReservedName(const std::string& relPath);

    // UTF-8 <-> UTF-16 helpers.
    static std::wstring utf8ToWide(const std::string& s);
    static std::string wideToUtf8(const std::wstring& s);
};

} // namespace npsync
