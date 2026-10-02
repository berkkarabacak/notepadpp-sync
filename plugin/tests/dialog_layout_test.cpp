// Checks that plugin dialog copy is plain ASCII and that each dialog's
// dialog-unit client covers the pixel layout of its labels and buttons.
#include "DialogLayout.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        ++g_checks;                                                                                          \
        if (!(cond)) {                                                                                       \
            ++g_failures;                                                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                      \
        }                                                                                                    \
    } while (0)

static void checkMsg(bool cond, const std::string& msg) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("FAIL %s\n", msg.c_str());
    }
}

static std::string readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

static bool isIdent(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Wide string literals in the plugin source. UTF-8 punctuation compiled as
// the ANSI code page is what produced "Notepad++ Sync â€” Sign In".
static void scanWideStrings(const std::string& file, const char* name) {
    int line = 1;
    for (size_t i = 0; i < file.size();) {
        if (file[i] == '\n') {
            ++line;
            ++i;
            continue;
        }
        if (file[i] == '/' && i + 1 < file.size() && file[i + 1] == '/') {
            while (i < file.size() && file[i] != '\n')
                ++i;
            continue;
        }
        if (file[i] == '/' && i + 1 < file.size() && file[i + 1] == '*') {
            i += 2;
            while (i + 1 < file.size() && !(file[i] == '*' && file[i + 1] == '/')) {
                if (file[i] == '\n')
                    ++line;
                ++i;
            }
            if (i + 1 < file.size())
                i += 2;
            continue;
        }
        if (file[i] == 'L' && i + 1 < file.size() && file[i + 1] == '"') {
            int startLine = line;
            bool ascii = true;
            i += 2;
            while (i < file.size()) {
                unsigned char c = static_cast<unsigned char>(file[i]);
                if (c == '"')
                    break;
                if (c == '\n')
                    ++line;
                if (c == '\\') {
                    ++i;
                    if (i >= file.size())
                        break;
                    char e = file[i];
                    if (e == 'u' || e == 'U') {
                        int digits = e == 'u' ? 4 : 8;
                        int cp = 0;
                        bool ok = true;
                        for (int d = 0; d < digits; ++d) {
                            ++i;
                            if (i >= file.size()) {
                                ok = false;
                                break;
                            }
                            int h = hexValue(file[i]);
                            if (h < 0) {
                                ok = false;
                                break;
                            }
                            cp = cp * 16 + h;
                        }
                        if (!ok || cp > 127)
                            ascii = false;
                    }
                    else if (e == 'x') {
                        int cp = 0;
                        int digits = 0;
                        while (i + 1 < file.size() && hexValue(file[i + 1]) >= 0 && digits < 4) {
                            ++i;
                            cp = cp * 16 + hexValue(file[i]);
                            ++digits;
                        }
                        if (cp > 127)
                            ascii = false;
                    }
                    ++i;
                    continue;
                }
                if (c >= 128)
                    ascii = false;
                ++i;
            }
            std::ostringstream msg;
            msg << name << ":" << startLine << " wide string contains a non-ASCII character";
            checkMsg(ascii, msg.str());
            if (i < file.size() && file[i] == '"')
                ++i;
            continue;
        }
        ++i;
    }
}

static const std::map<std::string, npsync::layout::DialogSize>& frames() {
    using namespace npsync::layout;
    static const std::map<std::string, DialogSize> all = {
        {"kPrompt", kPrompt},       {"kSignIn", kSignIn},     {"kStatus", kStatus},
        {"kPair", kPair},           {"kDevices", kDevices},   {"kSyncedFiles", kSyncedFiles},
        {"kConflicts", kConflicts}, {"kSettings", kSettings},
    };
    return all;
}

static const std::map<std::string, std::wstring>& symbols() {
    using namespace npsync::layout;
    static const std::map<std::string, std::wstring> all = {
        {"kSignInLead", kSignInLead},
        {"kCreateAccount", kCreateAccount},
        {"kAddFolder", kAddFolder},
        {"kAddFile", kAddFile},
        {"kRemoveSelected", kRemoveSelected},
        {"kSavePatterns", kSavePatterns},
        {"kPairLead", kPairLead},
        {"kPairMenu", kPairMenu},
    };
    return all;
}

static bool decodeOneString(const std::string& arg, size_t& i, std::wstring& out, std::string& err) {
    if (i + 1 >= arg.size() || arg[i] != 'L' || arg[i + 1] != '"') {
        err = "expected L\"...\"";
        return false;
    }
    i += 2;
    while (i < arg.size()) {
        char c = arg[i];
        if (c == '"') {
            ++i;
            return true;
        }
        if (c == '\\') {
            ++i;
            if (i >= arg.size()) {
                err = "truncated escape";
                return false;
            }
            char e = arg[i++];
            if (e == 'n' || e == 'r' || e == 't' || e == '\\' || e == '"' || e == '\'')
                out.push_back(static_cast<wchar_t>(e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e));
            else {
                err = "unsupported escape in control text";
                return false;
            }
            continue;
        }
        if (static_cast<unsigned char>(c) >= 128) {
            err = "non-ASCII control text";
            return false;
        }
        out.push_back(static_cast<wchar_t>(c));
        ++i;
    }
    err = "unterminated string";
    return false;
}

static bool decodeTextArg(const std::string& arg, std::wstring& out, std::string& err) {
    size_t i = 0;
    out.clear();
    while (i < arg.size()) {
        while (i < arg.size() && std::isspace(static_cast<unsigned char>(arg[i])))
            ++i;
        if (i >= arg.size())
            break;
        if (arg.compare(i, 8, "layout::") == 0) {
            i += 8;
            std::string name;
            while (i < arg.size() && isIdent(arg[i]))
                name.push_back(arg[i++]);
            auto it = symbols().find(name);
            if (it == symbols().end()) {
                err = "unknown layout symbol " + name;
                return false;
            }
            out += it->second;
            continue;
        }
        if (!decodeOneString(arg, i, out, err))
            return false;
    }
    return true;
}

static bool evalExpr(const std::string& raw, const std::map<std::string, int>& locals, int& out,
                     std::string& err) {
    std::string e = trim(raw);
    if (e.empty()) {
        err = "empty expression";
        return false;
    }
    int depth = 0;
    int split = -1;
    char op = 0;
    for (int i = static_cast<int>(e.size()) - 1; i >= 1; --i) {
        char c = e[static_cast<size_t>(i)];
        if (c == ')')
            ++depth;
        else if (c == '(')
            --depth;
        else if (depth == 0 && (c == '+' || c == '-')) {
            split = i;
            op = c;
            break;
        }
    }
    if (split > 0) {
        int left = 0;
        int right = 0;
        if (!evalExpr(e.substr(0, static_cast<size_t>(split)), locals, left, err))
            return false;
        if (!evalExpr(e.substr(static_cast<size_t>(split) + 1), locals, right, err))
            return false;
        out = op == '+' ? left + right : left - right;
        return true;
    }
    bool digits = !e.empty();
    for (char c : e)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            digits = false;
    if (digits) {
        out = std::stoi(e);
        return true;
    }
    auto local = locals.find(e);
    if (local != locals.end()) {
        out = local->second;
        return true;
    }
    if (e == "layout::kCheckHeight") {
        out = npsync::layout::kCheckHeight;
        return true;
    }
    if (e.compare(0, 8, "layout::") == 0 && e.find('(') == std::string::npos) {
        auto dot = e.find('.');
        if (dot == std::string::npos) {
            err = "bad layout field " + e;
            return false;
        }
        std::string name = e.substr(8, dot - 8);
        std::string field = e.substr(dot + 1);
        auto it = frames().find(name);
        if (it == frames().end()) {
            err = "unknown dialog " + name;
            return false;
        }
        if (field == "pixelW") {
            out = it->second.pixelW;
            return true;
        }
        if (field == "pixelH") {
            out = it->second.pixelH;
            return true;
        }
        err = "unknown field " + field;
        return false;
    }
    const char* fns[] = {"pushButtonPx", "checkBoxPx", "labelPx"};
    for (const char* fn : fns) {
        std::string prefix = std::string("layout::") + fn + "(";
        if (e.compare(0, prefix.size(), prefix) != 0)
            continue;
        if (e.back() != ')') {
            err = "unclosed " + prefix;
            return false;
        }
        std::string arg = e.substr(prefix.size(), e.size() - prefix.size() - 1);
        std::wstring text;
        if (!decodeTextArg(arg, text, err))
            return false;
        if (std::string(fn) == "pushButtonPx")
            out = npsync::layout::pushButtonPx(text.c_str());
        else if (std::string(fn) == "checkBoxPx")
            out = npsync::layout::checkBoxPx(text.c_str());
        else
            out = npsync::layout::labelPx(text.c_str());
        return true;
    }
    err = "cannot evaluate '" + e + "'";
    return false;
}

static std::vector<std::string> splitArgs(const std::string& file, size_t openParen, size_t& endOut) {
    std::vector<std::string> args;
    std::string cur;
    int depth = 0;
    bool inStr = false;
    for (size_t i = openParen; i < file.size(); ++i) {
        char c = file[i];
        if (inStr) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < file.size()) {
                cur.push_back(file[++i]);
            }
            else if (c == '"') {
                inStr = false;
            }
            continue;
        }
        if (c == '"') {
            inStr = true;
            cur.push_back(c);
            continue;
        }
        if (c == '(') {
            if (depth > 0)
                cur.push_back(c);
            ++depth;
            continue;
        }
        if (c == ')') {
            --depth;
            if (depth == 0) {
                args.push_back(trim(cur));
                endOut = i + 1;
                return args;
            }
            cur.push_back(c);
            continue;
        }
        if (c == ',' && depth == 1) {
            args.push_back(trim(cur));
            cur.clear();
            continue;
        }
        if (depth >= 1)
            cur.push_back(c);
    }
    endOut = file.size();
    return args;
}

struct CallSig
{
    const char* name;
    int text;
    int x;
    int y;
    int w;
    int h;
    bool button;
    bool check;
    bool label;
};

static void checkControl(const std::string& frameName, const CallSig& sig,
                         const std::vector<std::string>& args, const std::map<std::string, int>& locals,
                         int& checked) {
    auto frameIt = frames().find(frameName);
    if (frameIt == frames().end()) {
        checkMsg(false, "unknown dialog frame " + frameName);
        return;
    }
    const auto& frame = frameIt->second;
    auto need = [&](int index) {
        if (index < 0 || index >= static_cast<int>(args.size())) {
            checkMsg(false, std::string(sig.name) + " in " + frameName + " is missing an argument");
            return false;
        }
        return true;
    };
    if (!need(sig.x) || !need(sig.y) || !need(sig.w))
        return;
    int x = 0, y = 0, w = 0, h = 0;
    std::string err;
    if (!evalExpr(args[static_cast<size_t>(sig.x)], locals, x, err) ||
        !evalExpr(args[static_cast<size_t>(sig.y)], locals, y, err) ||
        !evalExpr(args[static_cast<size_t>(sig.w)], locals, w, err)) {
        checkMsg(false, frameName + " " + sig.name + ": " + err);
        return;
    }
    if (sig.h < 0)
        h = npsync::layout::kCheckHeight;
    else if (!need(sig.h) || !evalExpr(args[static_cast<size_t>(sig.h)], locals, h, err)) {
        checkMsg(false, frameName + " " + sig.name + " height: " + err);
        return;
    }
    std::ostringstream where;
    where << frameName << " " << sig.name << " at " << x << "," << y << " " << w << "x" << h;
    checkMsg(x >= 0 && y >= 0, where.str() + " has a negative origin");
    checkMsg(x + w <= frame.pixelW,
             where.str() + " extends past the dialog width " + std::to_string(frame.pixelW));
    checkMsg(y + h <= frame.pixelH,
             where.str() + " extends past the dialog height " + std::to_string(frame.pixelH));

    std::wstring text;
    if (sig.text >= 0 && sig.text < static_cast<int>(args.size())) {
        if (!decodeTextArg(args[static_cast<size_t>(sig.text)], text, err)) {
            checkMsg(false, frameName + " " + sig.name + " text: " + err);
            return;
        }
        checkMsg(npsync::layout::isAscii(text.c_str()), where.str() + " text is not ASCII");
    }
    if (text.empty()) {
        ++checked;
        return;
    }
    bool leftAligned = sig.check;
    if (sig.button) {
        for (const auto& arg : args) {
            if (arg.find("AUTOCHECKBOX") != std::string::npos ||
                arg.find("AUTORADIOBUTTON") != std::string::npos)
                leftAligned = true;
        }
    }
    if (sig.button && !leftAligned) {
        int needW = npsync::layout::pushButtonPx(text.c_str());
        checkMsg(w >= needW,
                 where.str() + " button is narrower than its caption (" + std::to_string(needW) + ")");
    }
    if (sig.check || leftAligned) {
        int needW = npsync::layout::checkBoxPx(text.c_str());
        checkMsg(w >= needW,
                 where.str() + " check/radio is narrower than its caption (" + std::to_string(needW) + ")");
    }
    if (sig.label) {
        int needW = npsync::layout::labelPx(text.c_str());
        int have = h >= 36 ? w * 2 : w;
        checkMsg(have >= needW,
                 where.str() + " label is narrower than its text (" + std::to_string(needW) + ")");
    }
    ++checked;
}

static void parseDeclarations(const std::string& file, size_t i, std::map<std::string, int>& locals,
                              size_t& endOut) {
    size_t j = i;
    while (j < file.size() && file[j] != ';')
        ++j;
    std::string decl = file.substr(i, j - i);
    endOut = j < file.size() ? j + 1 : j;
    // "const int tx = 16, ty = 48"
    auto eq = decl.find("int");
    if (eq == std::string::npos)
        return;
    std::string rest = decl.substr(eq + 3);
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (char c : rest) {
        if (c == '(')
            ++depth;
        if (c == ')')
            --depth;
        if (c == ',' && depth == 0) {
            parts.push_back(cur);
            cur.clear();
        }
        else
            cur.push_back(c);
    }
    if (!cur.empty())
        parts.push_back(cur);
    for (const auto& part : parts) {
        auto equal = part.find('=');
        if (equal == std::string::npos)
            continue;
        std::string name = trim(part.substr(0, equal));
        std::string expr = part.substr(equal + 1);
        int value = 0;
        std::string err;
        if (evalExpr(expr, locals, value, err))
            locals[name] = value;
    }
}

static void scanControls(const std::string& file) {
    const CallSig sigs[] = {
        {"makeLabel", 1, 2, 3, 4, 5, false, false, true},
        {"makeEdit", -1, 2, 3, 4, 5, false, false, false},
        {"makeButton", 2, 3, 4, 5, 6, true, false, false},
        {"makeCheck", 2, 3, 4, 5, -1, false, true, false},
        {"CreateWindowExW", -1, 4, 5, 6, 7, false, false, false},
    };
    std::string frame;
    std::map<std::string, int> locals;
    std::map<std::string, int> seen;
    int checked = 0;
    for (size_t i = 0; i < file.size();) {
        if (file.compare(i, 16, "// dialog-frame:") == 0) {
            i += 16;
            frame.clear();
            locals.clear();
            while (i < file.size() && isIdent(file[i]))
                frame.push_back(file[i++]);
            seen[frame] = 0;
            continue;
        }
        if (file.compare(i, 2, "//") == 0) {
            while (i < file.size() && file[i] != '\n')
                ++i;
            continue;
        }
        if (file.compare(i, 2, "/*") == 0) {
            i += 2;
            while (i + 1 < file.size() && !(file[i] == '*' && file[i + 1] == '/'))
                ++i;
            if (i + 1 < file.size())
                i += 2;
            continue;
        }
        if (!frame.empty() && file.compare(i, 10, "const int ") == 0 && (i == 0 || !isIdent(file[i - 1]))) {
            parseDeclarations(file, i, locals, i);
            continue;
        }
        bool matched = false;
        for (const auto& sig : sigs) {
            size_t len = std::char_traits<char>::length(sig.name);
            if (i + len < file.size() && file.compare(i, len, sig.name) == 0 && file[i + len] == '(' &&
                (i == 0 || !isIdent(file[i - 1]))) {
                if (frame.empty()) {
                    i += len;
                    matched = true;
                    break;
                }
                size_t end = i;
                auto args = splitArgs(file, i + len, end);
                int before = checked;
                checkControl(frame, sig, args, locals, checked);
                seen[frame] += checked - before;
                i = end;
                matched = true;
                break;
            }
        }
        if (!matched)
            ++i;
    }
    for (const auto& frameName : frames()) {
        auto it = seen.find(frameName.first);
        checkMsg(it != seen.end() && it->second > 0, "no controls checked for " + frameName.first);
    }
    checkMsg(checked >= 40, "expected to measure the dialog controls");
}

static void testWizardDoesNotOpenFolderList(const std::string& file) {
    auto start = file.find("void Dialogs::showFirstRunWizard");
    auto end = file.find("void Dialogs::showAbout");
    checkMsg(start != std::string::npos && end != std::string::npos && end > start,
             "wizard function not found");
    if (start == std::string::npos || end == std::string::npos || end <= start)
        return;
    std::string body = file.substr(start, end - start);
    checkMsg(body.find("showSyncedFiles") == std::string::npos,
             "first-run setup must not open the synced-files window");
    checkMsg(body.find("Name this device") == std::string::npos, "setup should end at the recovery key");
    checkMsg(body.find("Are your notes already on another computer?") != std::string::npos,
             "setup question was removed");
    checkMsg(body.find("Yes - I will open Notepad++ there") != std::string::npos, "Yes answer was removed");
    checkMsg(body.find("No - this is the first computer") != std::string::npos, "No answer was removed");
    checkMsg(body.find("makeEdit") == std::string::npos, "setup must not add a server URL field");
    checkMsg(body.find("Backend URL") == std::string::npos, "setup must not add a server URL field");
    checkMsg(body.find("showSignIn") != std::string::npos, "setup must still sign in");
    checkMsg(body.find("getMyNotes") != std::string::npos, "setup must still pair when notes are elsewhere");
    checkMsg(body.find("finishFirstRunKeys") != std::string::npos, "setup must still run the key step");
}

// Closing Notepad++ runs NPPN_SHUTDOWN on the UI thread, inside WM_CLOSE.
// The sync threads must be cancelled before they are joined, and a remote
// file must not SendMessage that same UI thread.
static void testCloseDoesNotBlockTheUiThread(const std::string& plugin, const std::string& engine,
                                             const std::string& api) {
    auto applied = plugin.find("onRemoteFileApplied");
    auto started = plugin.find("g_engine->start()", applied);
    checkMsg(applied != std::string::npos && started != std::string::npos && started > applied,
             "remote-file callback not found");
    if (applied != std::string::npos && started != std::string::npos && started > applied) {
        std::string body = plugin.substr(applied, started - applied);
        checkMsg(body.find("PostMessageW") != std::string::npos,
                 "a remote file must be posted to the UI thread");
        checkMsg(body.find("SendMessageW") == std::string::npos,
                 "a remote file must not SendMessage the UI thread during close");
    }
    checkMsg(plugin.find("PeekMessageW") != std::string::npos,
             "shutdown must drop a queued reload instead of leaving it behind");

    auto stop = engine.find("void SyncEngine::stop()");
    auto after = engine.find("std::string SyncEngine::deviceId()", stop);
    checkMsg(stop != std::string::npos && after != std::string::npos && after > stop, "stop() not found");
    if (stop != std::string::npos && after != std::string::npos && after > stop) {
        std::string body = engine.substr(stop, after - stop);
        auto cancel = body.find("cancelRequests()");
        auto join = body.find("workerThread_.join");
        checkMsg(cancel != std::string::npos && join != std::string::npos && cancel < join,
                 "stop() must cancel HTTP before joining the worker");
        checkMsg(body.find("ws_->stop()") != std::string::npos, "stop() must stop the websocket");
    }

    auto wsStop = api.find("void WsClient::stop()");
    auto wsRun = api.find("void WsClient::run()", wsStop);
    checkMsg(wsStop != std::string::npos && wsRun != std::string::npos && wsRun > wsStop,
             "WsClient::stop not found");
    if (wsStop != std::string::npos && wsRun != std::string::npos && wsRun > wsStop) {
        std::string body = api.substr(wsStop, wsRun - wsStop);
        auto close = body.find("closeWsHandles");
        auto join = body.find("join()");
        checkMsg(close != std::string::npos && join != std::string::npos && close < join,
                 "websocket stop must close the socket before join");
    }
}

int main() {
    using namespace npsync::layout;
    CHECK(frameFits(kPrompt));
    CHECK(frameFits(kSignIn));
    CHECK(frameFits(kStatus));
    CHECK(frameFits(kPair));
    CHECK(frameFits(kDevices));
    CHECK(frameFits(kSyncedFiles));
    CHECK(frameFits(kConflicts));
    CHECK(frameFits(kSettings));
    CHECK(isAscii(kTitleSignIn));
    CHECK(isAscii(kTitleSyncedFiles));
    CHECK(isAscii(kTitleRecovery));
    CHECK(isAscii(kTitleSetup));
    CHECK(legacyClientPx(kSignIn.pixelW) < 16 + labelPx(kSignInLead));
    CHECK(fromDlgX(toDlgX(kSignIn.pixelW)) >= 16 + labelPx(kSignInLead));
    CHECK(pushButtonPx(kRemoveSelected) > 130);
    CHECK(pushButtonPx(kAddFolder) > 110);
    CHECK(pushButtonPx(kSavePatterns) > 110);
    CHECK(checkBoxPx(kCreateAccount) > 314);

    const std::string dialogs = readFile(NPSYNC_DIALOGS_CPP);
    const std::string menu = readFile(NPSYNC_PLUGIN_DEFINITION_CPP);
    const std::string engine = readFile(NPSYNC_SYNC_ENGINE_CPP);
    const std::string api = readFile(NPSYNC_API_CLIENT_CPP);
    checkMsg(!dialogs.empty(), std::string("could not read ") + NPSYNC_DIALOGS_CPP);
    checkMsg(!menu.empty(), std::string("could not read ") + NPSYNC_PLUGIN_DEFINITION_CPP);
    checkMsg(!engine.empty(), std::string("could not read ") + NPSYNC_SYNC_ENGINE_CPP);
    checkMsg(!api.empty(), std::string("could not read ") + NPSYNC_API_CLIENT_CPP);
    if (!dialogs.empty()) {
        scanWideStrings(dialogs, "Dialogs.cpp");
        scanControls(dialogs);
        testWizardDoesNotOpenFolderList(dialogs);
        checkMsg(dialogs.find("layout::toDlgX(w)") != std::string::npos, "dialog template must use toDlgX");
        checkMsg(dialogs.find("layout::toDlgY(h)") != std::string::npos, "dialog template must use toDlgY");
        checkMsg(dialogs.find("w / 2") == std::string::npos,
                 "dialog width must not use the old pixel/2 units");
        checkMsg(dialogs.find("h / 2") == std::string::npos,
                 "dialog height must not use the old pixel/2 units");
    }
    if (!menu.empty()) {
        scanWideStrings(menu, "PluginDefinition.cpp");
        checkMsg(menu.find("Synced Files/Folders") != std::string::npos,
                 "the folder list must stay on the plugin menu");
        if (!engine.empty() && !api.empty())
            testCloseDoesNotBlockTheUiThread(menu, engine, api);
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
