#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "novaboot/http3/request.h"

namespace novaboot::http {

struct MultipartError {
    std::string message;
};

struct MultipartLimits {
    std::size_t max_total_bytes = 10U * 1024U * 1024U;
    std::size_t max_parts = 128;
    std::size_t max_header_bytes = 16U * 1024U;
    std::size_t max_field_bytes = 256U * 1024U;
    /// Files at or below this size remain owned in memory; larger files use a
    /// mode-0600 temporary file that is removed when the final UploadedFile
    /// owner is destroyed.
    std::size_t max_in_memory_file_bytes = 256U * 1024U;
    std::filesystem::path temporary_directory{};
};

namespace detail { struct MultipartTemporaryFile; }
class MultipartForm;

class UploadedFile {
public:
    [[nodiscard]] std::string_view field_name() const noexcept { return field_name_; }
    [[nodiscard]] std::string_view filename() const noexcept { return filename_; }
    [[nodiscard]] std::string_view content_type() const noexcept { return content_type_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool in_memory() const noexcept { return temporary_file_ == nullptr; }
    [[nodiscard]] std::string_view data() const noexcept { return data_; }
    /// A private, mode-0600 path for a spilled upload. It remains valid only
    /// while this object (or a copy) lives; move/copy it into durable storage
    /// before returning the request result to another lifetime.
    [[nodiscard]] const std::filesystem::path& temporary_path() const noexcept;

private:
    friend class MultipartForm;
    friend std::expected<MultipartForm, MultipartError>
    parse_multipart(const http3::Request&, const MultipartLimits&);

    std::string field_name_;
    std::string filename_;
    std::string content_type_;
    std::size_t size_ = 0;
    std::string data_;
    std::shared_ptr<detail::MultipartTemporaryFile> temporary_file_;
};

class MultipartForm {
public:
    [[nodiscard]] std::optional<std::string_view> field(std::string_view name) const;
    [[nodiscard]] const std::vector<std::string>* fields(std::string_view name) const;
    [[nodiscard]] const std::vector<UploadedFile>& files() const noexcept { return files_; }
    [[nodiscard]] std::vector<const UploadedFile*> files(std::string_view field_name) const;

private:
    friend std::expected<MultipartForm, MultipartError>
    parse_multipart(const http3::Request&, const MultipartLimits&);

    std::unordered_map<std::string, std::vector<std::string>> fields_;
    std::vector<UploadedFile> files_;
};

/// Parses a completed multipart/form-data request. It rejects malformed
/// boundaries, unsupported content types, size-limit violations, and unsafe
/// part headers without creating application-visible files.
[[nodiscard]] std::expected<MultipartForm, MultipartError>
parse_multipart(const http3::Request& request,
                const MultipartLimits& limits = {});

} // namespace novaboot::http
