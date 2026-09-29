#pragma once

#include <cstdint>
#include <utility>
#include <set>
#include <string>
#include <vector>

namespace cr {

enum class Side { Old = 0, New = 1 };

struct Location {
    std::string file;   // relative to the snapshot root, or absolute when `external`
    int line = 0;       // 1-based
    int col = 0;        // 1-based (bytes)
    int endLine = 0;    // inclusive, 0 = same as line
    bool external = false;

    bool valid() const { return line > 0 && !file.empty(); }
};

// ---------------------------------------------------------------------------
// Parsed source model (extracted from libclang, independent of it afterwards)

enum class TokKind : uint8_t { Punct, Keyword, Ident, Literal, Comment };

struct Token {
    std::string text;
    TokKind kind = TokKind::Punct;
    int line = 0;
    int col = 0;
    unsigned offset = 0;
};

// A statement inside a function body. Nested blocks are flattened with parent links.
struct Stmt {
    int tokBegin = 0, tokEnd = 0; // token indices into ParsedFile::tokens, [begin, end)
    int beginLine = 0, endLine = 0;
    int parent = -1;              // index into Entity::stmts, -1 for top level of the body
    int block = 0;                // id of the enclosing { } block (unique within the entity)
    int indexInBlock = 0;
    int depth = 0;
    uint64_t exactHash = 0;
    uint64_t normHash = 0;        // hash with identifiers alpha-renamed ($1, $2, ...)
};

enum class EntityKind { Function, Class, Enum, Variable };

struct Entity {
    EntityKind kind = EntityKind::Function;
    std::string kindName;      // "method", "constructor", "class template", ...
    std::string name;          // simple name
    std::string qualifiedName; // ns::Class::name
    std::string scope;         // ns::Class
    bool isMember = false;     // declared inside a class (method, static member...)
    std::string params;        // "(int, const std::string &) const"
    std::string signature;     // "int ns::Class::name(int, const std::string &) const"
    std::string usr;
    std::string file;
    int beginLine = 0, endLine = 0;
    int tokBegin = 0, tokEnd = 0;
    int bodyTokBegin = -1, bodyTokEnd = -1;
    std::vector<Stmt> stmts;
    std::set<std::string> calls; // names that appear as `name(`
    // Token ranges excluded from this entity's hashes (bodies of nested entities, e.g.
    // inline methods of a class), so that editing a method doesn't mark its class modified.
    std::vector<std::pair<int, int>> skipRanges;
    uint64_t exactHash = 0;
    uint64_t normHash = 0;

    bool isFunction() const { return kind == EntityKind::Function; }
    int tokenCount() const { return tokEnd - tokBegin; }
};

struct ParsedFile {
    std::string path; // relative
    std::vector<Token> tokens;
    std::vector<Entity> entities;
    int errorCount = 0;
    std::vector<std::string> diagnostics;
    bool parsed = false;
};

// ---------------------------------------------------------------------------
// Semantic changes

enum class ChangeKind {
    Added,
    Removed,
    Modified,
    Renamed,          // same body, different name
    Moved,            // same entity, different file or scope
    Reordered,        // same entity, moved within the file
    SignatureChanged, // same name, different parameters
    Extracted,        // new function made from statements of an existing one
    Inlined,          // removed function whose statements were put into a caller
    MovedCode,        // statements moved between functions
    CopiedCode,       // statements duplicated into another function
    SymbolRenamed,    // identifier consistently renamed across the change
    MovedLines,       // text-level moved block (not tied to an entity)
};

const char* changeKindName(ChangeKind k);

// A location that belongs to a change: an occurrence of a rename, a call site of an extracted
// function, a hunk of a modified function... Either side may be invalid.
struct RelatedItem {
    std::string label;  // "renamed", "call", "extracted code", "caller not updated", ...
    Location oldLoc;
    Location newLoc;
    bool warning = false; // worth a reviewer's attention
    bool onOldSide = false; // the item is about the base revision (newLoc is only context)
};

// Token ids used for comparisons. Comments and `skip` ranges are left out. With `normalized`,
// identifiers are alpha-renamed in order of first appearance ($1, $2, ...), so code that
// differs only by consistent renaming produces the same sequence.
std::vector<uint64_t> tokenIds(const std::vector<Token>& toks, int begin, int end, bool normalized,
                               const std::vector<std::pair<int, int>>* skip = nullptr);
uint64_t hashIds(const std::vector<uint64_t>& ids);

struct SemanticChange {
    ChangeKind kind = ChangeKind::Modified;
    std::string title;   // one line, e.g. "Renamed function foo → bar"
    std::string detail;  // optional longer explanation
    Location oldLoc;     // where it was (may be invalid for Added)
    Location newLoc;     // where it is now (may be invalid for Removed)
    double similarity = 0.0;
    bool trivial = false; // e.g. modified only by identifier renames; can be de-emphasized
    std::string oldName, newName;
    bool isFunction = false;            // the change is about a function (usages are calls)
    std::vector<RelatedItem> related;    // occurrences, call sites, hunks... in reading order
    std::string highlightOld, highlightNew; // identifier to highlight on each side
    // For class members: the class (simple name) and the ranges of the class and its members on
    // each side. Unqualified uses only count inside these ranges.
    std::string memberClassOld, memberClassNew;
    std::vector<Location> memberScopeOld, memberScopeNew;
    // Additional ranges (e.g. extracted statements), used for highlighting.
    std::vector<Location> relatedOld, relatedNew;
};

} // namespace cr
