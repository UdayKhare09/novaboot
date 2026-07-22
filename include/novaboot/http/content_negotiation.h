#pragma once

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "novaboot/http3/request.h"

namespace novaboot::http {

namespace detail {

inline std::string_view trim_media_token(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    const auto end = value.find_last_not_of(" \t");
    return value.substr(begin, end - begin + 1);
}

inline bool equals_ignore_case(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](char a, char b) {
                          const auto lower = [](char value) {
                              return value >= 'A' && value <= 'Z'
                                  ? static_cast<char>(value - 'A' + 'a')
                                  : value;
                          };
                          return lower(a) == lower(b);
                      });
}

inline std::optional<double> quality(std::string_view parameters) {
    double result = 1.0;
    while (!parameters.empty()) {
        const auto separator = parameters.find(';');
        const auto parameter = trim_media_token(parameters.substr(0, separator));
        if (separator == std::string_view::npos) {
            parameters = {};
        } else {
            parameters.remove_prefix(separator + 1);
        }

        const auto equals = parameter.find('=');
        if (equals == std::string_view::npos) continue;
        const auto key = trim_media_token(parameter.substr(0, equals));
        const auto value = trim_media_token(parameter.substr(equals + 1));
        if (!equals_ignore_case(key, "q")) continue;

        std::string owned(value);
        char* consumed = nullptr;
        result = std::strtod(owned.c_str(), &consumed);
        if (consumed != owned.c_str() + owned.size() || result < 0.0 || result > 1.0) {
            return std::nullopt;
        }
    }
    return result;
}

inline bool media_range_matches(std::string_view range, std::string_view offered) {
    const auto range_slash = range.find('/');
    const auto offered_slash = offered.find('/');
    if (range_slash == std::string_view::npos || offered_slash == std::string_view::npos ||
        range.find('/', range_slash + 1) != std::string_view::npos ||
        offered.find('/', offered_slash + 1) != std::string_view::npos) {
        return false;
    }
    const auto range_type = trim_media_token(range.substr(0, range_slash));
    const auto range_subtype = trim_media_token(range.substr(range_slash + 1));
    const auto offered_type = trim_media_token(offered.substr(0, offered_slash));
    const auto offered_subtype = trim_media_token(offered.substr(offered_slash + 1));
    return (range_type == "*" || equals_ignore_case(range_type, offered_type)) &&
           (range_subtype == "*" || equals_ignore_case(range_subtype, offered_subtype));
}

inline std::string_view media_type_only(std::string_view value) {
    const auto separator = value.find(';');
    return trim_media_token(value.substr(0, separator));
}

} // namespace detail

/// Select an offered representation using the request's RFC 9110 Accept
/// header. Client quality values take precedence; ties keep server offer order.
/// An absent or blank Accept header accepts the first offered representation.
[[nodiscard]] inline std::optional<std::string_view> negotiate_content_type(
    const http3::Request& request,
    const std::vector<std::string_view>& offered) {
    if (offered.empty()) return std::nullopt;
    const auto accept = request.headers().get("accept");
    if (!accept || detail::trim_media_token(*accept).empty()) return offered.front();

    double best_quality = -1.0;
    std::optional<std::string_view> result;
    std::size_t range_start = 0;
    while (range_start <= accept->size()) {
        const auto comma = accept->find(',', range_start);
        const auto segment = detail::trim_media_token(
            accept->substr(range_start, comma == std::string_view::npos
                ? std::string_view::npos : comma - range_start));
        if (!segment.empty()) {
            const auto semicolon = segment.find(';');
            const auto range = detail::trim_media_token(segment.substr(0, semicolon));
            const auto q = detail::quality(semicolon == std::string_view::npos
                ? std::string_view{} : segment.substr(semicolon + 1));
            if (q && *q > 0.0) {
                for (const auto candidate : offered) {
                    if (*q > best_quality && detail::media_range_matches(range, candidate)) {
                        best_quality = *q;
                        result = candidate;
                    }
                }
            }
        }
        if (comma == std::string_view::npos) break;
        range_start = comma + 1;
    }
    return result;
}

[[nodiscard]] inline bool accepts_content_type(const http3::Request& request,
                                                std::string_view content_type) {
    return negotiate_content_type(request, {detail::media_type_only(content_type)}).has_value();
}

} // namespace novaboot::http
