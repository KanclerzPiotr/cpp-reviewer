#include "CompileDb.hpp"

#include "Json.hpp"
#include "Snapshot.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace cr {

namespace {

std::vector<std::string> shellSplit(const std::string& cmd)
{
    std::vector<std::string> out;
    std::string cur;
    bool inWord = false;
    char quote = 0;
    for (size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (quote) {
            if (c == quote) {
                quote = 0;
            } else if (c == '\\' && quote == '"' && i + 1 < cmd.size()) {
                cur += cmd[++i];
            } else {
                cur += c;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
            inWord = true;
        } else if (c == '\\' && i + 1 < cmd.size()) {
            cur += cmd[++i];
            inWord = true;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (inWord)
                out.push_back(cur);
            cur.clear();
            inWord = false;
        } else {
            cur += c;
            inWord = true;
        }
    }
    if (inWord)
        out.push_back(cur);
    return out;
}

std::string absolutize(const std::string& p, const std::string& dir)
{
    fs::path path(p);
    if (path.is_absolute())
        return path.lexically_normal().string();
    return (fs::path(dir) / path).lexically_normal().string();
}

// absolutize() for the directory of one entry, remembering results: a database repeats the same
// include directories in every entry, and normalizing them each time dominates loading.
class Absolutizer {
public:
    const std::string& operator()(const std::string& p, const std::string& dir)
    {
        key_.assign(dir).push_back('\0');
        key_.append(p);
        auto it = cache_.find(key_);
        if (it == cache_.end())
            it = cache_.emplace(key_, absolutize(p, dir)).first;
        return it->second;
    }

private:
    std::string key_;
    std::unordered_map<std::string, std::string> cache_;
};

// Keeps only flags that matter for parsing and makes path arguments absolute.
std::vector<std::string> filterArgs(const std::vector<std::string>& raw, const std::string& dir,
                                    const std::string& file, Absolutizer& abs)
{
    static const std::set<std::string> dropWithValue = {"-o", "-MF", "-MT", "-MQ", "--serialize-diagnostics"};
    static const std::set<std::string> dropAlone = {"-c", "-MD", "-MMD", "-M", "-MM", "-MP", "-S", "-E",
                                                    "-fcolor-diagnostics", "-fdiagnostics-color", "--"};
    static const std::set<std::string> pathWithValue = {"-I", "-isystem", "-iquote", "-idirafter", "-include",
                                                        "-imacros", "-isysroot", "--sysroot"};
    std::vector<std::string> out;
    const auto absFile = absolutize(file, dir);
    const auto fileName = fs::path(file).filename().string();
    for (size_t i = 1; i < raw.size(); ++i) { // skip compiler executable
        const auto& a = raw[i];
        if (dropWithValue.count(a)) {
            ++i;
            continue;
        }
        if (dropAlone.count(a) || a.rfind("-fdiagnostics-color", 0) == 0 || a.rfind("-Werror", 0) == 0)
            continue;
        // The source file itself (only arguments ending with its name can be).
        if (a == file || (a.ends_with(fileName) && absolutize(a, dir) == absFile))
            continue;
        if (pathWithValue.count(a) && i + 1 < raw.size()) {
            out.push_back(a);
            out.push_back(abs(raw[++i], dir));
            continue;
        }
        bool handled = false;
        for (const char* p : {"-I", "-isystem", "-iquote"}) {
            std::string pre(p);
            if (a.size() > pre.size() && a.rfind(pre, 0) == 0) {
                out.push_back(pre + abs(a.substr(pre.size()), dir));
                handled = true;
                break;
            }
        }
        if (!handled)
            out.push_back(a);
    }
    return out;
}

} // namespace

std::string CompileDatabase::find(const std::string& projectRoot)
{
    std::error_code ec;
    fs::path root(projectRoot);
    for (auto candidate : {root / "compile_commands.json", root / "build" / "compile_commands.json",
                           root / "out" / "compile_commands.json"}) {
        if (fs::is_regular_file(candidate, ec))
            return candidate.string();
    }
    // build/<config>/, cmake-build-*/, out/build/<preset>/
    for (auto dirName : {"build", "out", "out/build", "."}) {
        auto d = root / dirName;
        if (!fs::is_directory(d, ec))
            continue;
        for (auto& e : fs::directory_iterator(d, ec)) {
            if (!e.is_directory(ec))
                continue;
            auto name = e.path().filename().string();
            if (std::string(dirName) == "." && name.rfind("cmake-build-", 0) != 0 && name.rfind("build", 0) != 0)
                continue;
            auto c = e.path() / "compile_commands.json";
            if (fs::is_regular_file(c, ec))
                return c.string();
        }
    }
    return {};
}

bool CompileDatabase::load(const std::string& jsonPath, std::string* error)
{
    std::ifstream in(jsonPath);
    if (!in) {
        if (error)
            *error = "cannot open " + jsonPath;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    JsonValue root;
    if (!parseJson(ss.str(), root, error))
        return false;
    entries_.clear();
    byDir_.clear();
    path_ = jsonPath;
    Absolutizer abs;
    for (const auto& item : root.arr()) {
        const auto& dir = item["directory"].str();
        const auto& file = item["file"].str();
        if (file.empty())
            continue;
        std::vector<std::string> raw;
        if (item["arguments"].isArray()) {
            for (const auto& a : item["arguments"].arr())
                raw.push_back(a.str());
        } else {
            raw = shellSplit(item["command"].str());
        }
        auto absFile = absolutize(file, dir);
        entries_[absFile] = Entry{filterArgs(raw, dir, file, abs)};
        byDir_.emplace(fs::path(absFile).parent_path().string(), absFile);
    }
    return true;
}

// Points a path argument ("/p", "-I/p", "-include /p" value...) under `from` into the snapshot at `to`.
// Paths that only exist in the project, such as the build directory with generated headers and
// downloaded dependencies, aren't part of an exported commit and keep pointing into the project.
std::string CompileDatabase::remap(const std::string& arg, const std::string& from, const std::string& to) const
{
    auto pos = arg.find(from);
    if (pos == std::string::npos)
        return arg;
    const auto rest = arg.substr(pos + from.size());
    if (!rest.empty() && rest.front() != '/')
        return arg; // "/repo-2" is not inside "/repo"
    auto candidate = to + rest;
    bool exists;
    {
        std::lock_guard lock(existsMutex_);
        auto it = exists_.find(candidate);
        if (it != exists_.end()) {
            exists = it->second;
        } else {
            std::error_code ec;
            exists = fs::exists(candidate, ec);
            exists_.emplace(candidate, exists);
        }
    }
    return exists ? arg.substr(0, pos) + candidate : arg;
}

const CompileDatabase::Entry* CompileDatabase::lookup(const std::string& absFile, bool allowGuess) const
{
    if (entries_.empty())
        return nullptr;
    if (auto it = entries_.find(absFile); it != entries_.end())
        return &it->second;

    // Headers: try a source file with the same stem, then any file in the same or a parent directory.
    fs::path p(absFile);
    for (const char* ext : {".cpp", ".cc", ".cxx", ".c++", ".c"}) {
        auto sibling = p;
        sibling.replace_extension(ext);
        if (auto it = entries_.find(sibling.string()); it != entries_.end())
            return &it->second;
    }
    for (auto dir = p.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (auto it = byDir_.find(dir.string()); it != byDir_.end())
            return &entries_.at(it->second);
        if (dir == dir.root_path())
            break;
    }
    if (!allowGuess)
        return nullptr;
    // A sibling "src" directory usually shares flags with "include".
    return &entries_.begin()->second;
}

bool CompileDatabase::covers(const std::string& projectRoot, const std::string& relPath) const
{
    return lookup((fs::path(projectRoot) / relPath).lexically_normal().string(), false) != nullptr;
}

std::vector<std::string> CompileDatabase::argsFor(const std::string& projectRoot, const std::string& snapshotRoot,
                                                  const std::string& relPath) const
{
    auto absInProject = (fs::path(projectRoot) / relPath).lexically_normal().string();
    const Entry* e = lookup(absInProject);
    if (!e)
        return fallbackArgs(snapshotRoot, relPath);

    std::vector<std::string> args;
    const bool inSnapshot = fs::path(projectRoot).lexically_normal() != fs::path(snapshotRoot).lexically_normal();
    auto from = fs::path(projectRoot).lexically_normal().string();
    auto to = fs::path(snapshotRoot).lexically_normal().string();
    for (size_t i = 0; i < e->args.size(); ++i) {
        const auto& a = e->args[i];
        if (a == "-x") { // language is decided below
            ++i;
            continue;
        }
        if (a.rfind("-x", 0) == 0)
            continue;
        args.push_back(inSnapshot ? remap(a, from, to) : a);
    }
    // Includes of the snapshot must win over the (possibly generated) ones from the build dir.
    args.push_back("-I" + to);
    const bool isC = fs::path(relPath).extension() == ".c";
    args.insert(args.begin(), {"-x", isC ? "c" : (isHeaderFile(relPath) ? "c++-header" : "c++")});
    args.push_back("-Wno-pragma-once-outside-header");
    args.push_back("-Wno-unknown-warning-option");
    args.push_back("-Qunused-arguments");
    return args;
}

std::vector<std::string> CompileDatabase::fallbackArgs(const std::string& snapshotRoot, const std::string& relPath)
{
    std::vector<std::string> args;
    auto ext = fs::path(relPath).extension().string();
    if (ext == ".c") {
        args = {"-x", "c", "-std=c17"};
    } else {
        args = {"-x", isHeaderFile(relPath) ? "c++-header" : "c++", "-std=c++20"};
    }
    fs::path root(snapshotRoot);
    args.push_back("-I" + root.string());
    std::error_code ec;
    for (auto sub : {"include", "src", "source", "lib"}) {
        if (fs::is_directory(root / sub, ec))
            args.push_back("-I" + (root / sub).string());
    }
    // Top-level components often have their own include directory (e.g. libfoo/include).
    for (auto& e : fs::directory_iterator(root, ec)) {
        if (e.is_directory(ec) && fs::is_directory(e.path() / "include", ec))
            args.push_back("-I" + (e.path() / "include").string());
    }
    // The file's own directory and its parent: helps with "module/foo.h" style includes.
    auto fileDir = (root / relPath).parent_path();
    args.push_back("-I" + fileDir.string());
    if (fileDir != root)
        args.push_back("-I" + fileDir.parent_path().string());
    args.push_back("-Wno-pragma-once-outside-header");
    return args;
}

} // namespace cr
