#include "NxMatrix/Util/Json.hpp"

namespace NxMatrix::Util {

std::string CanonicalJson(const nlohmann::json &Value) {
    // Objects are std::map inside nlohmann, so dump() already emits keys in
    // sorted order; indent -1 gives no whitespace; replace keeps invalid
    // UTF-8 in content strings from aborting the dump (signing must not throw).
    return Value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string JsonGetString(const nlohmann::json &Object, const std::string &Key, const std::string &Fallback) {
    if(!Object.is_object()) {
        return Fallback;
    }
    const auto It = Object.find(Key);
    if(It == Object.end() || !It->is_string()) {
        return Fallback;
    }
    return It->get<std::string>();
}

std::int64_t JsonGetInt(const nlohmann::json &Object, const std::string &Key, std::int64_t Fallback) {
    if(!Object.is_object()) {
        return Fallback;
    }
    const auto It = Object.find(Key);
    if(It == Object.end() || !It->is_number()) {
        return Fallback;
    }
    return It->get<std::int64_t>();
}

bool JsonGetBool(const nlohmann::json &Object, const std::string &Key, bool Fallback) {
    if(!Object.is_object()) {
        return Fallback;
    }
    const auto It = Object.find(Key);
    if(It == Object.end() || !It->is_boolean()) {
        return Fallback;
    }
    return It->get<bool>();
}

} // namespace NxMatrix::Util