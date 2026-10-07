#pragma once

#include <string>
#include <utility>
#include <vector>

namespace SCMiner {

// A JSON value - just enough of a reader for the answers Bedrock takes in
// from the web (UEX's prices and terminals). Strings come out as UTF-8.
struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<JsonValue> items;                              // an array's
    std::vector<std::pair<std::string, JsonValue>> members;    // an object's, in order

    // An object's member by name; nullptr where there is none or this is no object
    const JsonValue* find(const std::string& key) const;
    // ...its number, or `fallback` where it is missing, null or no number
    double numberOr(const std::string& key, double fallback = 0) const;
    // ...its text, or `fallback` where it is missing, null or no string
    std::string textOr(const std::string& key, const std::string& fallback = "") const;
};

// False, and `why`, for anything that is not one JSON value with nothing but
// white space around it
bool parseJson(const std::string& text, JsonValue& out, std::string& why);

}  // namespace SCMiner
