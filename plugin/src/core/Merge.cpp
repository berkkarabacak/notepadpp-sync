// Merge.cpp — line-based three-way merge (diff3-style).
//
// Algorithm:
//   1. Compute the classic LCS edit script base->local and base->remote
//      (Myers would be nicer; LCS via dynamic programming is fine for the
//      sizes involved — Notepad++ notes are typically small; guard for very
//      large files by bailing to conflict mode).
//   2. Walk both scripts simultaneously. Regions changed by only one side are
//      taken verbatim. Regions changed by both are a conflict unless both
//      sides produced identical text (then take it once).
#include "Merge.h"

#include <algorithm>
#include <cstring>

namespace npsync {

namespace {

constexpr size_t kMaxAutoMergeCells = 16 * 1000 * 1000; // LCS table cell budget

struct Edit {
    // One changed region: base[aStart, aEnd) replaced by `lines`.
    size_t aStart = 0, aEnd = 0;
    std::vector<std::string> lines;
};

// Compute line edits transforming `from` into `to` via LCS backtracking.
std::vector<Edit> diffLines(const std::vector<std::string>& from,
                            const std::vector<std::string>& to, bool& ok) {
    ok = true;
    const size_t n = from.size(), m = to.size();
    std::vector<Edit> edits;
    if (n == 0 && m == 0) return edits;
    if (n > 0 && m > 0 && n * m > kMaxAutoMergeCells) { ok = false; return edits; }

    // LCS length table (n+1)x(m+1), row-major.
    std::vector<uint32_t> dp((n + 1) * (m + 1), 0);
    auto at = [&](size_t i, size_t j) -> uint32_t& { return dp[i * (m + 1) + j]; };
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            at(i, j) = (from[i] == to[j])
                ? at(i + 1, j + 1) + 1
                : std::max(at(i + 1, j), at(i, j + 1));

    // Backtrack from (0,0), grouping consecutive delete/insert operations
    // between matches into Edit regions.
    size_t i = 0, j = 0;
    bool open = false;       // currently inside a changed region
    Edit cur;
    auto closeRegion = [&]() {
        if (open) { cur.aEnd = i; edits.push_back(std::move(cur)); cur = Edit{}; open = false; }
    };
    while (i < n && j < m) {
        if (from[i] == to[j]) {
            closeRegion();
            ++i; ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            if (!open) { open = true; cur.aStart = i; }
            ++i; // delete from[i]
        } else {
            if (!open) { open = true; cur.aStart = i; }
            cur.lines.push_back(to[j]);
            ++j; // insert to[j]
        }
    }
    if (i < n) {
        if (!open) { open = true; cur.aStart = i; }
        i = n;
    }
    if (j < m) {
        if (!open) { open = true; cur.aStart = i; }
        for (; j < m; ++j) cur.lines.push_back(to[j]);
    }
    closeRegion();
    return edits;
}

} // namespace

std::vector<std::string> splitLines(const std::string& text, std::string& eolOut) {
    std::vector<std::string> lines;
    size_t crlf = 0, lfOnly = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') (i > 0 && text[i - 1] == '\r') ? ++crlf : ++lfOnly;
    }
    eolOut = (crlf >= lfOnly) ? "\r\n" : "\n";
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            size_t end = (i > 0 && text[i - 1] == '\r') ? i - 1 : i;
            lines.emplace_back(text.substr(start, end - start));
            start = i + 1;
        }
    }
    if (start < text.size()) lines.emplace_back(text.substr(start));
    else if (!text.empty() && text.back() == '\n') { /* trailing newline: no phantom line */ }
    return lines;
}

MergeResult ThreeWayMerge::merge(const std::string& base,
                                 const std::string& local,
                                 const std::string& remote) {
    MergeResult result;
    if (local == remote) { result.clean = true; result.merged = local; return result; }
    if (local == base)   { result.clean = true; result.merged = remote; return result; }
    if (remote == base)  { result.clean = true; result.merged = local;  return result; }

    std::string eol;
    std::vector<std::string> baseLines   = splitLines(base, eol);
    std::string eol2, eol3;
    std::vector<std::string> localLines  = splitLines(local, eol2);
    std::vector<std::string> remoteLines = splitLines(remote, eol3);

    bool okL, okR;
    auto editsL = diffLines(baseLines, localLines, okL);
    auto editsR = diffLines(baseLines, remoteLines, okR);
    if (!okL || !okR) return result; // too large to auto-merge safely

    // Overlay both edit sets onto base with a region sweep: at each step,
    // find the nearest upcoming edit start, copy unchanged base lines up to
    // it, then fold every overlapping edit from both sides into one region.
    std::string out;
    auto appendLines = [&](const std::vector<std::string>& ls) {
        for (const auto& l : ls) { out += l; out += eol; }
    };
    const size_t nBase = baseLines.size();
    size_t iL = 0, iR = 0, basePos = 0;
    bool conflict = false;

    auto baseSlice = [&](size_t a, size_t b) {
        std::vector<std::string> v;
        for (size_t k = a; k < b && k < nBase; ++k) v.push_back(baseLines[k]);
        return v;
    };

    while (basePos < nBase || iL < editsL.size() || iR < editsR.size()) {
        size_t nextStart = SIZE_MAX;
        if (iL < editsL.size()) nextStart = std::min(nextStart, editsL[iL].aStart);
        if (iR < editsR.size()) nextStart = std::min(nextStart, editsR[iR].aStart);

        if (nextStart == SIZE_MAX) {
            while (basePos < nBase) { out += baseLines[basePos++]; out += eol; }
            break;
        }
        while (basePos < nextStart && basePos < nBase) { out += baseLines[basePos++]; out += eol; }

        // Fold all overlapping edits into [nextStart, regionEnd).
        size_t regionEnd = nextStart;
        size_t sL = iL, sR = iR;
        for (;;) {
            bool grew = false;
            while (sL < editsL.size() && editsL[sL].aStart <= regionEnd) {
                regionEnd = std::max(regionEnd, editsL[sL].aEnd);
                ++sL; grew = true;
            }
            while (sR < editsR.size() && editsR[sR].aStart <= regionEnd) {
                regionEnd = std::max(regionEnd, editsR[sR].aEnd);
                ++sR; grew = true;
            }
            if (!grew) break;
        }
        bool hasL = sL > iL, hasR = sR > iR;

        // Build each side's replacement text for the region.
        auto build = [&](const std::vector<Edit>& edits, size_t from, size_t to) {
            std::vector<std::string> rep;
            size_t cursor = nextStart;
            for (size_t k = from; k < to; ++k) {
                while (cursor < edits[k].aStart && cursor < nBase) rep.push_back(baseLines[cursor++]);
                for (const auto& l : edits[k].lines) rep.push_back(l);
                cursor = edits[k].aEnd;
            }
            while (cursor < regionEnd && cursor < nBase) rep.push_back(baseLines[cursor++]);
            return rep;
        };

        std::vector<std::string> repL = hasL ? build(editsL, iL, sL) : baseSlice(nextStart, regionEnd);
        std::vector<std::string> repR = hasR ? build(editsR, iR, sR) : baseSlice(nextStart, regionEnd);

        if (!hasL && !hasR) {
            appendLines(baseSlice(nextStart, regionEnd));
        } else if (repL == repR) {
            appendLines(repL);
        } else if (!hasR) {
            appendLines(repL);
        } else if (!hasL) {
            appendLines(repR);
        } else {
            conflict = true;
            result.hunks.push_back(MergeHunk{repL, repR});
        }

        iL = sL; iR = sR;
        basePos = regionEnd;
    }

    if (conflict) return result;
    result.clean = true;
    result.merged = out;
    return result;
}

} // namespace npsync
