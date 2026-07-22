#include "novaboot/http/static_resource.h"

#include <charconv>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

namespace novaboot::http {
namespace {

std::string mime_type(const std::filesystem::path& path) {
    const auto extension = path.extension().string();
    if (extension == ".html" || extension == ".htm") return "text/html; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".js" || extension == ".mjs") return "application/javascript; charset=utf-8";
    if (extension == ".json") return "application/json";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".ico") return "image/x-icon";
    if (extension == ".txt") return "text/plain; charset=utf-8";
    if (extension == ".pdf") return "application/pdf";
    if (extension == ".xml") return "application/xml";
    return "application/octet-stream";
}

bool parse_size(std::string_view value, std::uintmax_t& parsed) {
    if (value.empty()) return false;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size();
}

struct ByteRange {
    std::uintmax_t first = 0;
    std::uintmax_t last = 0;
};

enum class RangeParse { Absent, Valid, Invalid };

RangeParse parse_range(std::string_view value, std::uintmax_t file_size, ByteRange& range) {
    if (value.empty()) return RangeParse::Absent;
    constexpr std::string_view prefix = "bytes=";
    if (!value.starts_with(prefix) || value.find(',') != std::string_view::npos || file_size == 0) {
        return RangeParse::Invalid;
    }
    value.remove_prefix(prefix.size());
    const auto dash = value.find('-');
    if (dash == std::string_view::npos || value.find('-', dash + 1) != std::string_view::npos) {
        return RangeParse::Invalid;
    }

    const auto start = value.substr(0, dash);
    const auto end = value.substr(dash + 1);
    if (start.empty()) {
        std::uintmax_t suffix = 0;
        if (!parse_size(end, suffix) || suffix == 0) return RangeParse::Invalid;
        suffix = std::min(suffix, file_size);
        range = {.first = file_size - suffix, .last = file_size - 1};
        return RangeParse::Valid;
    }

    std::uintmax_t first = 0;
    if (!parse_size(start, first) || first >= file_size) return RangeParse::Invalid;
    std::uintmax_t last = file_size - 1;
    if (!end.empty() && (!parse_size(end, last) || last < first)) return RangeParse::Invalid;
    range = {.first = first, .last = std::min(last, file_size - 1)};
    return RangeParse::Valid;
}

bool is_below(const std::filesystem::path& base, const std::filesystem::path& target) {
    const auto relative = target.lexically_relative(base);
    if (relative.empty()) return false;
    for (const auto& component : relative) {
        if (component == "..") return false;
    }
    return true;
}

std::string etag_for(std::uintmax_t size, std::filesystem::file_time_type modified) {
    return "\"" + std::to_string(size) + "-" +
           std::to_string(modified.time_since_epoch().count()) + "\"";
}

} // namespace

bool serve_static_resource(const std::filesystem::path& root,
                           const http3::Request& request,
                           Response& response,
                           bool head_only) {
    if (root.empty()) return false;
    try {
        const auto base = std::filesystem::canonical(root);
        auto request_path = request.path();
        if (const auto query = request_path.find('?'); query != std::string_view::npos) {
            request_path = request_path.substr(0, query);
        }
        if (request_path.empty() || request_path == "/") request_path = "/index.html";
        if (!request_path.starts_with('/') || request_path.find('\\') != std::string_view::npos ||
            request_path.find('\0') != std::string_view::npos) {
            return false;
        }

        const auto target = std::filesystem::weakly_canonical(
            base / std::filesystem::path(request_path.substr(1)));
        if (!is_below(base, target) || !std::filesystem::is_regular_file(target)) return false;

        const auto file_size = std::filesystem::file_size(target);
        const auto etag = etag_for(file_size, std::filesystem::last_write_time(target));
        if (const auto if_none_match = request.header("if-none-match");
            if_none_match && (*if_none_match == etag || *if_none_match == "*")) {
            response.status(304)
                .header("etag", etag)
                .header("cache-control", "public, max-age=3600")
                .header("accept-ranges", "bytes");
            return true;
        }

        ByteRange range{.first = 0, .last = file_size == 0 ? 0 : file_size - 1};
        const auto range_status = request.header("range")
            ? parse_range(*request.header("range"), file_size, range)
            : RangeParse::Absent;
        if (range_status == RangeParse::Invalid) {
            response.status(416)
                .header("content-range", "bytes */" + std::to_string(file_size))
                .header("accept-ranges", "bytes");
            return true;
        }

        const bool partial = range_status == RangeParse::Valid;
        const auto body_size = partial ? range.last - range.first + 1 : file_size;
        std::string body;
        if (!head_only) {
            std::ifstream file(target, std::ios::binary);
            if (!file) return false;
            file.seekg(static_cast<std::streamoff>(range.first));
            body.resize(static_cast<std::size_t>(body_size));
            file.read(body.data(), static_cast<std::streamsize>(body.size()));
            body.resize(static_cast<std::size_t>(file.gcount()));
            if (body.size() != body_size) return false;
        }

        response.status(partial ? 206 : 200)
            .header("content-type", mime_type(target))
            .header("content-length", std::to_string(body_size))
            .header("etag", etag)
            .header("cache-control", "public, max-age=3600")
            .header("accept-ranges", "bytes");
        if (partial) {
            response.header("content-range", "bytes " + std::to_string(range.first) + "-" +
                            std::to_string(range.last) + "/" + std::to_string(file_size));
        }
        if (!head_only) response.body(std::move(body));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace novaboot::http
