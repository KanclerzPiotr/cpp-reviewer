#include "Json.hpp"

#include <cstdlib>

namespace cr {

namespace {

const JsonValue kNull{};
const std::string kEmptyString;
const JsonValue::Array kEmptyArray;
const JsonValue::Object kEmptyObject;

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool parse(JsonValue& out, std::string* error)
    {
        bool ok = value(out);
        ws();
        if (ok && i_ != s_.size())
            ok = fail("trailing characters");
        if (!ok && error)
            *error = error_ + " at offset " + std::to_string(i_);
        return ok;
    }

private:
    bool fail(const char* msg)
    {
        if (error_.empty())
            error_ = msg;
        return false;
    }

    void ws()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t'))
            ++i_;
    }

    bool literal(const char* word)
    {
        size_t n = std::char_traits<char>::length(word);
        if (s_.compare(i_, n, word) != 0)
            return fail("bad literal");
        i_ += n;
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& cp)
    {
        if (i_ + 4 > s_.size())
            return fail("bad escape");
        cp = static_cast<unsigned>(std::strtoul(s_.substr(i_, 4).c_str(), nullptr, 16));
        i_ += 4;
        return true;
    }

    bool string(std::string& out)
    {
        ++i_; // opening quote
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size())
                break;
            char e = s_[i_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(i_, 2, "\\u") == 0) {
                    i_ += 2;
                    unsigned lo = 0;
                    if (!hex4(lo))
                        return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool value(JsonValue& out)
    {
        ws();
        if (i_ >= s_.size())
            return fail("unexpected end");
        char c = s_[i_];
        if (c == '{') {
            ++i_;
            auto obj = std::make_shared<JsonValue::Object>();
            ws();
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                out.v = obj;
                return true;
            }
            for (;;) {
                ws();
                if (i_ >= s_.size() || s_[i_] != '"')
                    return fail("expected key");
                std::string key;
                if (!string(key))
                    return false;
                ws();
                if (i_ >= s_.size() || s_[i_] != ':')
                    return fail("expected ':'");
                ++i_;
                JsonValue v;
                if (!value(v))
                    return false;
                (*obj)[key] = std::move(v);
                ws();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == '}') {
                    ++i_;
                    break;
                }
                return fail("expected ',' or '}'");
            }
            out.v = obj;
            return true;
        }
        if (c == '[') {
            ++i_;
            auto arr = std::make_shared<JsonValue::Array>();
            ws();
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                out.v = arr;
                return true;
            }
            for (;;) {
                JsonValue v;
                if (!value(v))
                    return false;
                arr->push_back(std::move(v));
                ws();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == ']') {
                    ++i_;
                    break;
                }
                return fail("expected ',' or ']'");
            }
            out.v = arr;
            return true;
        }
        if (c == '"') {
            std::string s;
            if (!string(s))
                return false;
            out.v = std::move(s);
            return true;
        }
        if (c == 't') {
            out.v = true;
            return literal("true");
        }
        if (c == 'f') {
            out.v = false;
            return literal("false");
        }
        if (c == 'n') {
            out.v = nullptr;
            return literal("null");
        }
        char* end = nullptr;
        double d = std::strtod(s_.c_str() + i_, &end);
        if (end == s_.c_str() + i_)
            return fail("unexpected character");
        i_ = static_cast<size_t>(end - s_.c_str());
        out.v = d;
        return true;
    }

    const std::string& s_;
    size_t i_ = 0;
    std::string error_;
};

} // namespace

const std::string& JsonValue::str() const
{
    auto p = std::get_if<std::string>(&v);
    return p ? *p : kEmptyString;
}

const JsonValue::Array& JsonValue::arr() const
{
    auto p = std::get_if<std::shared_ptr<Array>>(&v);
    return p ? **p : kEmptyArray;
}

const JsonValue::Object& JsonValue::obj() const
{
    auto p = std::get_if<std::shared_ptr<Object>>(&v);
    return p ? **p : kEmptyObject;
}

const JsonValue& JsonValue::operator[](const std::string& key) const
{
    const auto& o = obj();
    auto it = o.find(key);
    return it == o.end() ? kNull : it->second;
}

bool parseJson(const std::string& text, JsonValue& out, std::string* error)
{
    return Parser(text).parse(out, error);
}

} // namespace cr
