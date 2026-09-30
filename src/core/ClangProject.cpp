#include "ClangProject.hpp"

#include "Diff.hpp"

#include <clang-c/Index.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

namespace cr {

struct ClangProject::Tu {
    CXIndex index = nullptr;
    CXTranslationUnit tu = nullptr;
    std::mutex mutex;

    ~Tu()
    {
        if (tu)
            clang_disposeTranslationUnit(tu);
        if (index)
            clang_disposeIndex(index);
    }
};

namespace {

std::string toStd(CXString s)
{
    const char* c = clang_getCString(s);
    std::string r = c ? c : "";
    clang_disposeString(s);
    return r;
}

using VisitFn = std::function<CXChildVisitResult(CXCursor, CXCursor)>;

CXChildVisitResult trampoline(CXCursor c, CXCursor p, CXClientData d)
{
    return (*static_cast<VisitFn*>(d))(c, p);
}

void visitChildren(CXCursor c, VisitFn fn)
{
    clang_visitChildren(c, trampoline, &fn);
}

bool isFunctionKind(CXCursorKind k)
{
    return k == CXCursor_FunctionDecl || k == CXCursor_CXXMethod || k == CXCursor_Constructor ||
           k == CXCursor_Destructor || k == CXCursor_ConversionFunction || k == CXCursor_FunctionTemplate;
}

bool isClassKind(CXCursorKind k)
{
    return k == CXCursor_ClassDecl || k == CXCursor_StructDecl || k == CXCursor_UnionDecl ||
           k == CXCursor_ClassTemplate || k == CXCursor_ClassTemplatePartialSpecialization;
}

bool isScopeKind(CXCursorKind k)
{
    return k == CXCursor_Namespace || isClassKind(k) || k == CXCursor_EnumDecl || isFunctionKind(k);
}

const char* friendlyKind(CXCursorKind k)
{
    switch (k) {
    case CXCursor_FunctionDecl: return "function";
    case CXCursor_CXXMethod: return "method";
    case CXCursor_Constructor: return "constructor";
    case CXCursor_Destructor: return "destructor";
    case CXCursor_ConversionFunction: return "conversion operator";
    case CXCursor_FunctionTemplate: return "function template";
    case CXCursor_ClassDecl: return "class";
    case CXCursor_StructDecl: return "struct";
    case CXCursor_UnionDecl: return "union";
    case CXCursor_ClassTemplate: return "class template";
    case CXCursor_ClassTemplatePartialSpecialization: return "class template specialization";
    case CXCursor_EnumDecl: return "enum";
    case CXCursor_EnumConstantDecl: return "enumerator";
    case CXCursor_FieldDecl: return "field";
    case CXCursor_VarDecl: return "variable";
    case CXCursor_ParmDecl: return "parameter";
    case CXCursor_TypedefDecl: return "typedef";
    case CXCursor_TypeAliasDecl: return "type alias";
    case CXCursor_TypeAliasTemplateDecl: return "alias template";
    case CXCursor_Namespace: return "namespace";
    case CXCursor_NamespaceAlias: return "namespace alias";
    case CXCursor_MacroDefinition: return "macro";
    case CXCursor_TemplateTypeParameter: return "template parameter";
    case CXCursor_NonTypeTemplateParameter: return "template parameter";
    case CXCursor_ConceptDecl: return "concept";
    default: return "symbol";
    }
}

// Spelling without libclang's "(unnamed struct at /abs/path:1:2)" and template arguments.
std::string cleanName(CXCursor c)
{
    auto name = toStd(clang_getCursorSpelling(c));
    if (name.empty() || name.rfind("(unnamed", 0) == 0 || name.rfind("(anonymous", 0) == 0 ||
        clang_Cursor_isAnonymous(c))
        return "(anonymous)";
    auto k = clang_getCursorKind(c);
    if ((k == CXCursor_Constructor || k == CXCursor_Destructor) && name.back() == '>') {
        auto lt = name.find('<');
        if (lt != std::string::npos)
            name.resize(lt);
    }
    return name;
}

std::string qualifiedScope(CXCursor parent)
{
    std::vector<std::string> parts;
    while (!clang_Cursor_isNull(parent) && !clang_isInvalid(clang_getCursorKind(parent)) &&
           clang_getCursorKind(parent) != CXCursor_TranslationUnit) {
        auto k = clang_getCursorKind(parent);
        if (isScopeKind(k))
            parts.push_back(cleanName(parent));
        parent = clang_getCursorSemanticParent(parent);
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty())
            out += "::";
        out += *it;
    }
    return out;
}

// True when the cursor is written in (or expanded from a macro used in) `mainFile`.
// clang_Location_isFromMainFile rejects code produced by macros such as FMT_BEGIN_NAMESPACE.
bool inFile(CXCursor c, CXFile mainFile)
{
    CXFile f = nullptr;
    clang_getExpansionLocation(clang_getCursorLocation(c), &f, nullptr, nullptr, nullptr);
    return f && mainFile && clang_File_isEqual(f, mainFile);
}

struct Extent {
    unsigned beginOffset = 0, endOffset = 0;
    int beginLine = 0, endLine = 0;
};

Extent extentOf(CXCursor c)
{
    Extent e;
    CXSourceRange r = clang_getCursorExtent(c);
    unsigned line = 0, col = 0, off = 0;
    CXFile f = nullptr;
    clang_getExpansionLocation(clang_getRangeStart(r), &f, &line, &col, &off);
    e.beginOffset = off;
    e.beginLine = static_cast<int>(line);
    clang_getExpansionLocation(clang_getRangeEnd(r), &f, &line, &col, &off);
    e.endOffset = off;
    e.endLine = static_cast<int>(line);
    return e;
}

TokKind tokKind(CXTokenKind k)
{
    switch (k) {
    case CXToken_Punctuation: return TokKind::Punct;
    case CXToken_Keyword: return TokKind::Keyword;
    case CXToken_Identifier: return TokKind::Ident;
    case CXToken_Literal: return TokKind::Literal;
    case CXToken_Comment: return TokKind::Comment;
    }
    return TokKind::Punct;
}

uint64_t exactHash(const std::vector<Token>& toks, int b, int e,
                   const std::vector<std::pair<int, int>>* skip = nullptr)
{
    return hashIds(tokenIds(toks, b, e, false, skip));
}

uint64_t normHash(const std::vector<Token>& toks, int b, int e,
                  const std::vector<std::pair<int, int>>* skip = nullptr)
{
    return hashIds(tokenIds(toks, b, e, true, skip));
}

class Extractor {
public:
    Extractor(ParsedFile& pf, CXFile mainFile) : pf_(pf), mainFile_(mainFile) {}

    void run(CXCursor root)
    {
        visitScope(root);
        excludeNestedBodies();
    }

private:
    int tokenAt(unsigned offset) const
    {
        auto it = std::lower_bound(pf_.tokens.begin(), pf_.tokens.end(), offset,
                                   [](const Token& t, unsigned off) { return t.offset < off; });
        return static_cast<int>(it - pf_.tokens.begin());
    }

    void rangeOf(CXCursor c, int& tb, int& te, int& bl, int& el) const
    {
        auto ex = extentOf(c);
        tb = tokenAt(ex.beginOffset);
        te = tokenAt(ex.endOffset);
        bl = ex.beginLine;
        el = ex.endLine;
    }

    void visitScope(CXCursor parent)
    {
        visitChildren(parent, [&](CXCursor c, CXCursor) {
            if (!inFile(c, mainFile_))
                return CXChildVisit_Continue;
            auto k = clang_getCursorKind(c);
            if (k == CXCursor_Namespace || k == CXCursor_LinkageSpec || k == CXCursor_FriendDecl ||
                k == CXCursor_UnexposedDecl) {
                visitScope(c);
            } else if (isFunctionKind(k)) {
                if (clang_isCursorDefinition(c))
                    addEntity(c, EntityKind::Function);
            } else if (isClassKind(k)) {
                if (clang_isCursorDefinition(c)) {
                    addEntity(c, EntityKind::Class);
                    visitScope(c);
                }
            } else if (k == CXCursor_EnumDecl) {
                if (clang_isCursorDefinition(c))
                    addEntity(c, EntityKind::Enum);
            } else if (k == CXCursor_VarDecl) {
                addEntity(c, EntityKind::Variable);
            }
            return CXChildVisit_Continue;
        });
    }

    // A class's hash shouldn't change when only one of its inline method bodies changes.
    void excludeNestedBodies()
    {
        for (auto& cls : pf_.entities) {
            if (cls.kind != EntityKind::Class)
                continue;
            for (const auto& inner : pf_.entities) {
                if (&inner == &cls || inner.tokBegin < cls.tokBegin || inner.tokEnd > cls.tokEnd)
                    continue;
                if (inner.kind == EntityKind::Function && inner.bodyTokBegin >= 0)
                    cls.skipRanges.emplace_back(inner.bodyTokBegin, inner.bodyTokEnd);
                else if (inner.kind == EntityKind::Class && (inner.tokBegin > cls.tokBegin || inner.tokEnd < cls.tokEnd))
                    cls.skipRanges.emplace_back(inner.tokBegin, inner.tokEnd);
            }
            if (cls.skipRanges.empty())
                continue;
            std::sort(cls.skipRanges.begin(), cls.skipRanges.end());
            cls.exactHash = exactHash(pf_.tokens, cls.tokBegin, cls.tokEnd, &cls.skipRanges);
            cls.normHash = normHash(pf_.tokens, cls.tokBegin, cls.tokEnd, &cls.skipRanges);
        }
    }

    void addEntity(CXCursor c, EntityKind kind)
    {
        Entity e;
        auto k = clang_getCursorKind(c);
        e.kind = kind;
        e.kindName = friendlyKind(k);
        e.name = cleanName(c);
        e.scope = qualifiedScope(clang_getCursorSemanticParent(c));
        e.isMember = isClassKind(clang_getCursorKind(clang_getCursorSemanticParent(c)));
        e.qualifiedName = e.scope.empty() ? e.name : e.scope + "::" + e.name;
        e.usr = toStd(clang_getCursorUSR(c));
        e.file = pf_.path;
        rangeOf(c, e.tokBegin, e.tokEnd, e.beginLine, e.endLine);
        if (e.tokEnd <= e.tokBegin)
            return;

        if (kind == EntityKind::Function) {
            auto display = toStd(clang_getCursorDisplayName(c));
            if (auto p = display.find('(', display.rfind(e.name, 0) == 0 ? e.name.size() : 0); p != std::string::npos)
                e.params = display.substr(p);
            if (k == CXCursor_CXXMethod && clang_CXXMethod_isConst(c))
                e.params += " const";
            std::string result;
            if (k != CXCursor_Constructor && k != CXCursor_Destructor && k != CXCursor_ConversionFunction)
                result = toStd(clang_getTypeSpelling(clang_getCursorResultType(c)));
            e.signature = (result.empty() ? "" : result + " ") + e.qualifiedName + e.params;

            CXCursor body = clang_getNullCursor();
            visitChildren(c, [&](CXCursor ch, CXCursor) {
                if (clang_getCursorKind(ch) == CXCursor_CompoundStmt)
                    body = ch;
                return CXChildVisit_Continue;
            });
            if (!clang_Cursor_isNull(body)) {
                int bl = 0, el = 0;
                rangeOf(body, e.bodyTokBegin, e.bodyTokEnd, bl, el);
                int blockCounter = 0;
                collectBlock(body, -1, 0, e, blockCounter);
                for (int i = e.bodyTokBegin; i + 1 < e.bodyTokEnd; ++i)
                    if (pf_.tokens[i].kind == TokKind::Ident && pf_.tokens[i + 1].text == "(")
                        e.calls.insert(pf_.tokens[i].text);
            }
        } else {
            // Template specializations share the name of their primary template: keep the
            // arguments ("<int>") so they don't get confused with each other.
            auto display = toStd(clang_getCursorDisplayName(c));
            if (display.size() > e.name.size() && display.rfind(e.name, 0) == 0 && display[e.name.size()] == '<')
                e.params = display.substr(e.name.size());
            e.signature = e.kindName + " " + e.qualifiedName + e.params;
        }
        e.exactHash = exactHash(pf_.tokens, e.tokBegin, e.tokEnd);
        e.normHash = normHash(pf_.tokens, e.tokBegin, e.tokEnd);
        pf_.entities.push_back(std::move(e));
    }

    void collectBlock(CXCursor compound, int parent, int depth, Entity& e, int& blockCounter)
    {
        const int blockId = blockCounter++;
        int idx = 0;
        visitChildren(compound, [&](CXCursor ch, CXCursor) {
            Stmt s;
            rangeOf(ch, s.tokBegin, s.tokEnd, s.beginLine, s.endLine);
            // Include the trailing ';' that the statement extent leaves out.
            if (s.tokEnd < static_cast<int>(pf_.tokens.size()) && pf_.tokens[s.tokEnd].text == ";")
                ++s.tokEnd;
            if (s.tokEnd <= s.tokBegin)
                return CXChildVisit_Continue;
            s.parent = parent;
            s.block = blockId;
            s.indexInBlock = idx++;
            s.depth = depth;
            s.exactHash = exactHash(pf_.tokens, s.tokBegin, s.tokEnd);
            s.normHash = normHash(pf_.tokens, s.tokBegin, s.tokEnd);
            const int me = static_cast<int>(e.stmts.size());
            e.stmts.push_back(s);
            findNestedBlocks(ch, me, depth + 1, e, blockCounter);
            return CXChildVisit_Continue;
        });
    }

    void findNestedBlocks(CXCursor c, int parent, int depth, Entity& e, int& blockCounter)
    {
        visitChildren(c, [&](CXCursor ch, CXCursor) {
            if (clang_getCursorKind(ch) == CXCursor_CompoundStmt) {
                collectBlock(ch, parent, depth, e, blockCounter);
                return CXChildVisit_Continue;
            }
            return CXChildVisit_Recurse;
        });
    }

    ParsedFile& pf_;
    CXFile mainFile_;
};

} // namespace

ClangProject::ClangProject(Snapshot snapshot, std::shared_ptr<const CompileDatabase> db, std::string projectRoot)
    : snap_(std::move(snapshot)), db_(std::move(db)), projectRoot_(std::move(projectRoot))
{
}

ClangProject::~ClangProject() = default;

std::string ClangProject::absPath(const std::string& file) const
{
    if (fs::path(file).is_absolute())
        return fs::path(file).lexically_normal().string();
    return (fs::path(snap_.root) / file).lexically_normal().string();
}

std::vector<std::string> ClangProject::argsFor(const std::string& abs) const
{
    auto rel = snap_.relPath(abs);
    if (rel.empty()) // outside of the snapshot (e.g. a system header)
        return CompileDatabase::fallbackArgs(fs::path(abs).parent_path().string(), fs::path(abs).filename().string());
    if (db_ && !db_->empty())
        return db_->argsFor(projectRoot_, snap_.root, rel);
    return CompileDatabase::fallbackArgs(snap_.root, rel);
}

std::shared_ptr<ClangProject::Tu> ClangProject::acquireTu(const std::string& abs, bool skipBodies)
{
    if (!skipBodies) {
        std::lock_guard lock(tuMutex_);
        if (auto it = tus_.find(abs); it != tus_.end()) {
            tuLru_.remove(abs);
            tuLru_.push_front(abs);
            return it->second;
        }
    }

    unsigned opts = CXTranslationUnit_KeepGoing | CXTranslationUnit_IgnoreNonErrorsFromIncludedFiles;
    if (skipBodies)
        opts |= CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_Incomplete;
    else
        opts |= CXTranslationUnit_DetailedPreprocessingRecord;

    auto tryParse = [&](const std::vector<std::string>& args) -> std::shared_ptr<Tu> {
        auto t = std::make_shared<Tu>();
        t->index = clang_createIndex(0, 0);
        std::vector<const char*> argv;
        for (const auto& a : args)
            argv.push_back(a.c_str());
        auto err = clang_parseTranslationUnit2(t->index, abs.c_str(), argv.data(), static_cast<int>(argv.size()),
                                               nullptr, 0, opts, &t->tu);
        if (err != CXError_Success || !t->tu)
            return nullptr;
        return t;
    };

    auto args = argsFor(abs);
    auto t = tryParse(args);
    if (!t) {
        auto rel = snap_.relPath(abs);
        t = tryParse(CompileDatabase::fallbackArgs(snap_.root, rel.empty() ? abs : rel));
    }
    if (!t)
        return nullptr;

    if (!skipBodies) {
        std::lock_guard lock(tuMutex_);
        tus_[abs] = t;
        tuLru_.remove(abs);
        tuLru_.push_front(abs);
        while (tuLru_.size() > kMaxTus) {
            tus_.erase(tuLru_.back());
            tuLru_.pop_back();
        }
    }
    return t;
}

std::shared_ptr<const ParsedFile> ClangProject::parse(const std::string& relPath)
{
    {
        std::lock_guard lock(parsedMutex_);
        if (auto it = parsed_.find(relPath); it != parsed_.end())
            return it->second;
    }

    auto pf = std::make_shared<ParsedFile>();
    pf->path = relPath;
    const auto abs = absPath(relPath);
    auto t = fs::exists(abs) ? acquireTu(abs, false) : nullptr;
    if (t) {
        std::lock_guard tuLock(t->mutex);
        CXTranslationUnit tu = t->tu;

        const unsigned nd = clang_getNumDiagnostics(tu);
        for (unsigned i = 0; i < nd; ++i) {
            CXDiagnostic d = clang_getDiagnostic(tu, i);
            if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error) {
                ++pf->errorCount;
                if (pf->diagnostics.size() < 50)
                    pf->diagnostics.push_back(
                        toStd(clang_formatDiagnostic(d, clang_defaultDiagnosticDisplayOptions())));
            }
            clang_disposeDiagnostic(d);
        }

        CXFile file = clang_getFile(tu, abs.c_str());
        if (file) {
            size_t size = 0;
            clang_getFileContents(tu, file, &size);
            CXSourceRange range = clang_getRange(clang_getLocationForOffset(tu, file, 0),
                                                 clang_getLocationForOffset(tu, file, static_cast<unsigned>(size)));
            CXToken* toks = nullptr;
            unsigned n = 0;
            clang_tokenize(tu, range, &toks, &n);
            pf->tokens.reserve(n);
            for (unsigned i = 0; i < n; ++i) {
                Token t;
                t.kind = tokKind(clang_getTokenKind(toks[i]));
                t.text = toStd(clang_getTokenSpelling(tu, toks[i]));
                unsigned line = 0, col = 0, off = 0;
                CXFile f = nullptr;
                clang_getSpellingLocation(clang_getTokenLocation(tu, toks[i]), &f, &line, &col, &off);
                t.line = static_cast<int>(line);
                t.col = static_cast<int>(col);
                t.offset = off;
                pf->tokens.push_back(std::move(t));
            }
            clang_disposeTokens(tu, toks, n);

            Extractor(*pf, file).run(clang_getTranslationUnitCursor(tu));
            pf->parsed = true;
        }
    }

    std::lock_guard lock(parsedMutex_);
    auto [it, inserted] = parsed_.emplace(relPath, pf);
    return it->second;
}

Location ClangProject::toLocation(const std::string& absFile, int line, int col) const
{
    Location loc;
    auto norm = fs::path(absFile).lexically_normal().string();
    auto rel = snap_.relPath(norm);
    loc.file = rel.empty() ? norm : rel;
    loc.external = rel.empty();
    loc.line = line;
    loc.col = col;
    return loc;
}

namespace {

bool cursorFileLocation(CXCursor c, std::string& file, int& line, int& col)
{
    CXFile f = nullptr;
    unsigned l = 0, cl = 0;
    clang_getSpellingLocation(clang_getCursorLocation(c), &f, &l, &cl, nullptr);
    if (!f)
        return false;
    file = toStd(clang_getFileName(f));
    line = static_cast<int>(l);
    col = static_cast<int>(cl);
    return !file.empty();
}

} // namespace

std::optional<SymbolInfo> ClangProject::symbolAt(const std::string& file, int line, int col)
{
    const auto abs = absPath(file);
    auto t = acquireTu(abs, false);
    if (!t)
        return std::nullopt;
    std::lock_guard tuLock(t->mutex);
    CXTranslationUnit tu = t->tu;
    CXFile f = clang_getFile(tu, abs.c_str());
    if (!f)
        return std::nullopt;
    CXCursor c = clang_getCursor(tu, clang_getLocation(tu, f, static_cast<unsigned>(line), static_cast<unsigned>(col)));
    if (clang_Cursor_isNull(c) || clang_isInvalid(clang_getCursorKind(c)))
        return std::nullopt;

    SymbolInfo info;
    if (clang_getCursorKind(c) == CXCursor_InclusionDirective) {
        CXFile inc = clang_getIncludedFile(c);
        if (!inc)
            return std::nullopt;
        auto name = toStd(clang_getFileName(inc));
        info.kind = "include";
        info.spelling = name;
        info.includedFile = toLocation(name, 1, 1);
        info.definition = info.includedFile;
        return info;
    }

    CXCursor ref = clang_getCursorReferenced(c);
    if (clang_Cursor_isNull(ref) || clang_isInvalid(clang_getCursorKind(ref))) {
        if (!clang_isDeclaration(clang_getCursorKind(c)))
            return std::nullopt;
        ref = c;
    }
    const auto rk = clang_getCursorKind(ref);
    info.spelling = cleanName(ref);
    info.kind = friendlyKind(rk);
    info.usr = toStd(clang_getCursorUSR(ref));
    auto scope = qualifiedScope(clang_getCursorSemanticParent(ref));
    info.qualifiedName = scope.empty() ? info.spelling : scope + "::" + info.spelling;
    if (isFunctionKind(rk)) {
        std::string result;
        if (rk != CXCursor_Constructor && rk != CXCursor_Destructor)
            result = toStd(clang_getTypeSpelling(clang_getCursorResultType(ref)));
        auto display = toStd(clang_getCursorDisplayName(ref));
        auto p = display.find('(');
        info.type = (result.empty() ? "" : result + " ") + (scope.empty() ? "" : scope + "::") + display.substr(0, p) +
                    (p == std::string::npos ? "" : display.substr(p));
        if (rk == CXCursor_CXXMethod && clang_CXXMethod_isConst(ref))
            info.type += " const";
    } else if (rk != CXCursor_MacroDefinition && rk != CXCursor_Namespace) {
        info.type = toStd(clang_getTypeSpelling(clang_getCursorType(ref)));
    }
    info.comment = toStd(clang_Cursor_getRawCommentText(ref));

    std::string fileName;
    int l = 0, cl = 0;
    CXCursor def = clang_getCursorDefinition(ref);
    if (!clang_Cursor_isNull(def) && cursorFileLocation(def, fileName, l, cl))
        info.definition = toLocation(fileName, l, cl);
    CXCursor canon = clang_getCanonicalCursor(ref);
    if (!clang_Cursor_isNull(canon) && cursorFileLocation(canon, fileName, l, cl))
        info.declaration = toLocation(fileName, l, cl);
    else if (cursorFileLocation(ref, fileName, l, cl))
        info.declaration = toLocation(fileName, l, cl);

    if (!info.definition && !info.usr.empty()) {
        auto defs = definitionsOf(info.usr);
        if (!defs.empty())
            info.definition = defs.front();
    }
    return info;
}

namespace {

// On-disk cache of the declarations found in one file, keyed by content and compile flags.
// Format: "D" (definition) or "d" (declaration), line, column, file ("=" for the file itself),
// external flag, USR; tab separated, one per line. "#failed" marks files libclang can't parse.
constexpr const char* kIndexCacheVersion = "cppreviewer-index-2";

struct IndexEntry {
    bool definition;
    Location loc;
    std::string usr;
};

std::string indexCachePath(uint64_t key)
{
    char name[24];
    std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(key));
    return (fs::path(cacheDirectory()) / "symbols" / std::string(name, 2) / name).string();
}

bool readIndexCache(const std::string& path, const std::string& self, std::vector<IndexEntry>& out, bool& failed)
{
    std::ifstream in(path);
    if (!in)
        return false;
    std::string line;
    failed = false;
    while (std::getline(in, line)) {
        if (line == "#failed") {
            failed = true;
            continue;
        }
        std::vector<std::string> f;
        size_t b = 0;
        for (int k = 0; k < 5; ++k) {
            auto e = line.find('\t', b);
            if (e == std::string::npos)
                return false;
            f.push_back(line.substr(b, e - b));
            b = e + 1;
        }
        IndexEntry entry;
        entry.definition = f[0] == "D";
        entry.loc.line = std::atoi(f[1].c_str());
        entry.loc.col = std::atoi(f[2].c_str());
        entry.loc.file = f[3] == "=" ? self : f[3];
        entry.loc.external = f[4] == "1";
        entry.usr = line.substr(b);
        out.push_back(std::move(entry));
    }
    return true;
}

void writeIndexCache(const std::string& path, const std::string& self, const std::vector<IndexEntry>& entries, bool failed)
{
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const auto tmp = path + ".tmp" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (failed)
            out << "#failed\n";
        for (const auto& e : entries)
            out << (e.definition ? 'D' : 'd') << '\t' << e.loc.line << '\t' << e.loc.col << '\t'
                << (e.loc.file == self ? "=" : e.loc.file) << '\t' << (e.loc.external ? '1' : '0') << '\t' << e.usr << '\n';
        if (!out)
            return;
    }
    fs::rename(tmp, path, ec);
    if (ec)
        fs::remove(tmp, ec);
}

} // namespace

void ClangProject::buildIndex(const std::function<void(int, int)>& progress, const std::atomic<bool>& cancel)
{
    auto files = listCppFiles(snap_);
    // With a compile database, skip code it doesn't build (other components, other platforms):
    // parsing it with borrowed flags is slow, useless, and it is what makes libclang crash.
    if (db_ && !db_->empty())
        std::erase_if(files, [&](const std::string& f) { return !db_->covers(projectRoot_, f); });
    const int total = static_cast<int>(files.size());
    std::atomic<int> next{0}, done{0};
    std::mutex progressMutex;

    auto worker = [&] {
        for (;;) {
            if (cancel)
                return;
            int i = next++;
            if (i >= total)
                return;
            const auto& rel = files[static_cast<size_t>(i)];
            const auto abs = absPath(rel);
            // Cache key: content and flags, with this snapshot's location taken out so the same
            // file in another revision hits the same entry.
            std::string content;
            {
                std::ifstream in(abs, std::ios::binary);
                content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            uint64_t key = hashCombine(hashString(kIndexCacheVersion), hashString(content));
            for (const auto& a : argsFor(abs)) {
                std::string norm = a;
                if (auto p = norm.find(snap_.root); p != std::string::npos)
                    norm.replace(p, snap_.root.size(), "$SNAPSHOT");
                key = hashCombine(key, hashString(norm));
            }
            const auto cachePath = indexCachePath(key);
            std::vector<IndexEntry> entries;
            bool failed = false;
            const bool cached = readIndexCache(cachePath, rel, entries, failed);
            std::shared_ptr<Tu> t = cached ? nullptr : acquireTu(abs, true);
            if (!cached && !t)
                failed = true;
            if (t) {
                CXFile mainFile = clang_getFile(t->tu, abs.c_str());
                visitChildren(clang_getTranslationUnitCursor(t->tu), [&](CXCursor c, CXCursor) {
                    if (!inFile(c, mainFile))
                        return CXChildVisit_Continue;
                    auto k = clang_getCursorKind(c);
                    if (!clang_isDeclaration(k) || k == CXCursor_ParmDecl || k == CXCursor_TemplateTypeParameter ||
                        k == CXCursor_NonTypeTemplateParameter || k == CXCursor_TemplateTemplateParameter ||
                        k == CXCursor_UsingDirective || k == CXCursor_UsingDeclaration)
                        return CXChildVisit_Recurse;
                    auto usr = toStd(clang_getCursorUSR(c));
                    std::string fileName;
                    int l = 0, cl = 0;
                    if (!usr.empty() && cursorFileLocation(c, fileName, l, cl)) {
                        bool definition = clang_isCursorDefinition(c) != 0;
                        // With skipped function bodies libclang doesn't call functions definitions and
                        // their extent stops before the body: look for "{", ": init" or "try" after it.
                        if (!definition && isFunctionKind(k)) {
                            CXFile f = nullptr;
                            unsigned end = 0;
                            clang_getSpellingLocation(clang_getRangeEnd(clang_getCursorExtent(c)), &f, nullptr, nullptr, &end);
                            if (f && clang_File_isEqual(f, mainFile)) {
                                size_t p = end;
                                while (p < content.size() && std::isspace(static_cast<unsigned char>(content[p])))
                                    ++p;
                                definition = p < content.size() &&
                                             (content[p] == '{' || content[p] == ':' || content.compare(p, 3, "try") == 0);
                            }
                        }
                        entries.push_back({definition, toLocation(fileName, l, cl), usr});
                    }
                    return CXChildVisit_Recurse;
                });
            }
            if (!cached && !cancel)
                writeIndexCache(cachePath, rel, entries, failed);
            if (cached)
                ++cacheHits_;
            ++indexedFiles_;
            if (!entries.empty()) {
                std::lock_guard lock(indexMutex_);
                for (auto& e : entries)
                    (e.definition ? defs_ : decls_)[e.usr].push_back(std::move(e.loc));
            }
            int d = ++done;
            if (progress) {
                std::lock_guard lock(progressMutex);
                progress(d, total);
            }
        }
    };

    unsigned n = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < n; ++i)
        threads.emplace_back(worker);
    for (auto& th : threads)
        th.join();
    if (!cancel)
        indexReady_ = true;
}

std::vector<Location> ClangProject::definitionsOf(const std::string& usr) const
{
    std::lock_guard lock(indexMutex_);
    auto it = defs_.find(usr);
    return it == defs_.end() ? std::vector<Location>{} : it->second;
}

std::vector<Location> ClangProject::declarationsOf(const std::string& usr) const
{
    std::lock_guard lock(indexMutex_);
    auto it = decls_.find(usr);
    return it == decls_.end() ? std::vector<Location>{} : it->second;
}

} // namespace cr
