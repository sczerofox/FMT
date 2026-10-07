#include "fmt/common/envelope.hpp"

#include <utility>

namespace fmt {

nlohmann::json envelope_ok(nlohmann::json data) {
    nlohmann::json value = nlohmann::json::object();
    value["ok"] = true;
    value["data"] = std::move(data);
    return value;
}

nlohmann::json envelope_error(const Error& error) {
    nlohmann::json detail = nlohmann::json::object();
    detail["code"] = code_string(error.code);
    detail["message"] = error.message;

    nlohmann::json value = nlohmann::json::object();
    value["ok"] = false;
    value["error"] = std::move(detail);
    return value;
}

}  // namespace fmt
