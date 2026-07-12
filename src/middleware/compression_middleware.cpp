#include "novaboot/middleware/compression_middleware.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

#include <zlib.h>

namespace novaboot::middleware {

namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool accepts_gzip(http3::Request& req) {
    const auto header = req.header("accept-encoding");
    if (!header) return false;

    const auto value = lower(*header);
    std::size_t start = 0;
    while (start < value.size()) {
        const auto comma = value.find(',', start);
        const auto end = comma == std::string::npos ? value.size() : comma;
        auto token = std::string_view(value).substr(start, end - start);

        while (!token.empty() &&
               std::isspace(static_cast<unsigned char>(token.front()))) {
            token.remove_prefix(1);
        }
        while (!token.empty() &&
               std::isspace(static_cast<unsigned char>(token.back()))) {
            token.remove_suffix(1);
        }

        if (token.starts_with("gzip")) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }

    return false;
}

bool status_allows_body(int status) {
    return status >= 200 && status != 204 && status != 304;
}

bool content_type_matches(std::string_view content_type,
                          const std::vector<std::string>& prefixes) {
    const auto normalized = lower(content_type);
    for (const auto& prefix : prefixes) {
        if (normalized.starts_with(lower(prefix))) return true;
    }
    return false;
}

bool response_is_compressible(http3::Response& res,
                              const CompressionMiddleware::Config& cfg) {
    if (!status_allows_body(res.status_code())) return false;
    if (res.body_size() < cfg.min_size_bytes) return false;
    if (res.headers().has("content-encoding")) return false;

    const auto content_type = res.headers().get("content-type");
    if (!content_type) return false;

    return content_type_matches(*content_type,
                                cfg.compressible_content_types);
}

std::optional<std::string> gzip(std::string_view body, int level) {
    z_stream stream{};
    const int init_result = deflateInit2(
        &stream,
        level,
        Z_DEFLATED,
        MAX_WBITS + 16,
        8,
        Z_DEFAULT_STRATEGY);

    if (init_result != Z_OK) return std::nullopt;

    std::string compressed;
    compressed.resize(deflateBound(
        &stream,
        static_cast<uLong>(body.size())));

    stream.next_in = reinterpret_cast<Bytef*>(
        const_cast<char*>(body.data()));
    stream.avail_in = static_cast<uInt>(body.size());
    stream.next_out = reinterpret_cast<Bytef*>(compressed.data());
    stream.avail_out = static_cast<uInt>(compressed.size());

    const int result = deflate(&stream, Z_FINISH);
    if (result != Z_STREAM_END) {
        deflateEnd(&stream);
        return std::nullopt;
    }

    compressed.resize(stream.total_out);
    deflateEnd(&stream);
    return compressed;
}

void append_vary(http3::Response& res, std::string_view value) {
    const auto current = res.headers().get("vary");
    if (!current || current->empty()) {
        res.header("Vary", value);
        return;
    }

    const auto normalized = lower(*current);
    if (normalized.find(lower(value)) != std::string::npos) return;
    res.headers().set("Vary", std::string(*current) + ", " + std::string(value));
}

} // namespace

CompressionMiddleware::CompressionMiddleware()
    : CompressionMiddleware(Config{}) {}

CompressionMiddleware::CompressionMiddleware(Config cfg)
    : cfg_(std::move(cfg)) {}

void CompressionMiddleware::handle(http3::Request& req,
                                   http3::Response& res,
                                   context::RequestContext& ctx,
                                   Next next) {
    (void)ctx;

    next();

    if (!accepts_gzip(req) || !response_is_compressible(res, cfg_)) {
        return;
    }

    auto compressed = gzip(res.body_data(), cfg_.gzip_level);
    if (!compressed || compressed->size() >= res.body_size()) {
        return;
    }

    res.body(std::move(*compressed));
    res.headers().remove("content-length");
    res.header("Content-Encoding", "gzip");
    append_vary(res, "Accept-Encoding");
}

} // namespace novaboot::middleware
