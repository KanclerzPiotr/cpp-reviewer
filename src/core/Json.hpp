#pragma once

#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace cr {

// Minimal JSON value, enough for compile_commands.json and small configs.
struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v;

    bool isString() const { return std::holds_alternative<std::string>(v); }
    bool isArray() const { return std::holds_alternative<std::shared_ptr<Array>>(v); }
    bool isObject() const { return std::holds_alternative<std::shared_ptr<Object>>(v); }

    const std::string& str() const;
    const Array& arr() const;
    const Object& obj() const;
    // Returns a null value when the key is missing or this isn't an object.
    const JsonValue& operator[](const std::string& key) const;
};

// Returns false on a syntax error.
bool parseJson(const std::string& text, JsonValue& out, std::string* error = nullptr);

} // namespace cr
