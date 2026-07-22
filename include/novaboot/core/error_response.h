#pragma once

#include <exception>
#include <format>
#include <string>
#include <string_view>

#include "novaboot/http3/response.h"

namespace novaboot::core {

/// Escape text for inclusion as a JSON string value.
[[nodiscard]] inline std::string json_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const unsigned char character : text) {
        switch (character) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (character < 0x20) {
                escaped += std::format("\\u{:04x}", static_cast<unsigned int>(character));
            } else {
                escaped.push_back(static_cast<char>(character));
            }
        }
    }
    return escaped;
}

/// Write the final fallback response for an exception not handled by an
/// application exception handler. Exception text is deliberately hidden by
/// default because it often contains implementation, database, or secret data.
inline void write_unhandled_error(http3::Response& response,
                                  const std::exception& error,
                                  bool expose_error_details = false) {
    if (expose_error_details) {
        response.status(500).json(
            R"({"error":"Internal Server Error","message":")" +
            json_escape(error.what()) + R"("})");
        return;
    }
    response.status(500).json(R"({"error":"Internal Server Error"})");
}

/// Fallback for a non-standard thrown value.
inline void write_unknown_error(http3::Response& response) {
    response.status(500).json(R"({"error":"Internal Server Error"})");
}

} // namespace novaboot::core
