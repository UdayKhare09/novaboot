#include "novaboot/http/multipart.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string_view>
#include <unistd.h>

namespace novaboot::http {
namespace detail {

struct MultipartTemporaryFile {
    explicit MultipartTemporaryFile(std::filesystem::path value) : path(std::move(value)) {}
    ~MultipartTemporaryFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    std::filesystem::path path;
};

} // namespace detail
namespace {

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

bool safe_token(std::string_view value) {
    if (value.empty()) return false;
    return std::ranges::none_of(value, [](unsigned char character) {
        return character < 0x21 || character == 0x7f || character == '"' || character == '\\';
    });
}

std::optional<std::string> parameter(std::string_view value, std::string_view wanted) {
    while (!value.empty()) {
        const auto separator = value.find(';');
        auto part = trim(value.substr(0, separator));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos && trim(part.substr(0, equals)) == wanted) {
            auto result = trim(part.substr(equals + 1));
            if (result.size() >= 2 && result.front() == '"' && result.back() == '"') {
                result.remove_prefix(1);
                result.remove_suffix(1);
            }
            if (result.find('\r') == std::string_view::npos && result.find('\n') == std::string_view::npos) {
                return std::string(result);
            }
            return std::nullopt;
        }
        if (separator == std::string_view::npos) break;
        value.remove_prefix(separator + 1);
    }
    return std::nullopt;
}

std::optional<std::string> boundary_from(std::string_view content_type) {
    const auto semicolon = content_type.find(';');
    if (trim(content_type.substr(0, semicolon)) != "multipart/form-data") return std::nullopt;
    auto boundary = parameter(semicolon == std::string_view::npos ? std::string_view{} :
                              content_type.substr(semicolon + 1), "boundary");
    if (!boundary || boundary->size() > 70 || !safe_token(*boundary)) return std::nullopt;
    return boundary;
}

bool valid_header_value(std::string_view value) {
    return std::ranges::none_of(value, [](unsigned char character) {
        return character == '\r' || character == '\n' || character == '\0';
    });
}

bool write_all(int fd, std::string_view content) {
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto written = ::write(fd, content.data() + offset, content.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

std::expected<std::shared_ptr<detail::MultipartTemporaryFile>, MultipartError>
spill_file(std::string_view content, const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        return std::unexpected(MultipartError{"Multipart temporary directory is not available"});
    }
    auto template_path = directory / "novaboot-upload-XXXXXX";
    auto mutable_path = template_path.string();
    const int fd = ::mkstemp(mutable_path.data());
    if (fd < 0) return std::unexpected(MultipartError{"Could not create multipart temporary file"});
    const bool written = write_all(fd, content);
    const int close_result = ::close(fd);
    if (!written || close_result != 0) {
        std::error_code remove_error;
        std::filesystem::remove(mutable_path, remove_error);
        return std::unexpected(MultipartError{"Could not write multipart temporary file"});
    }
    return std::make_shared<detail::MultipartTemporaryFile>(std::move(mutable_path));
}

} // namespace

const std::filesystem::path& UploadedFile::temporary_path() const noexcept {
    static const std::filesystem::path empty;
    return temporary_file_ ? temporary_file_->path : empty;
}

std::optional<std::string_view> MultipartForm::field(std::string_view name) const {
    const auto values = fields(name);
    if (!values || values->empty()) return std::nullopt;
    return values->front();
}

const std::vector<std::string>* MultipartForm::fields(std::string_view name) const {
    const auto found = fields_.find(std::string(name));
    return found == fields_.end() ? nullptr : &found->second;
}

std::vector<const UploadedFile*> MultipartForm::files(std::string_view field_name) const {
    std::vector<const UploadedFile*> result;
    for (const auto& file : files_) {
        if (file.field_name() == field_name) result.push_back(&file);
    }
    return result;
}

std::expected<MultipartForm, MultipartError>
parse_multipart(const http3::Request& request, const MultipartLimits& limits) {
    const auto content_type = request.header("content-type");
    if (!content_type) return std::unexpected(MultipartError{"Missing multipart Content-Type"});
    const auto boundary = boundary_from(*content_type);
    if (!boundary) return std::unexpected(MultipartError{"Invalid multipart boundary"});
    const auto body = request.body();
    if (body.size() > limits.max_total_bytes) {
        return std::unexpected(MultipartError{"Multipart body exceeds configured size limit"});
    }

    const auto temporary_directory = limits.temporary_directory.empty()
        ? std::filesystem::temp_directory_path()
        : limits.temporary_directory;
    const std::string delimiter = "--" + *boundary;
    const std::string next_delimiter = "\r\n" + delimiter;
    if (!body.starts_with(delimiter)) {
        return std::unexpected(MultipartError{"Multipart body does not start with boundary"});
    }

    MultipartForm form;
    std::size_t offset = 0;
    std::size_t parts = 0;
    while (true) {
        if (!body.substr(offset).starts_with(delimiter)) {
            return std::unexpected(MultipartError{"Malformed multipart boundary"});
        }
        offset += delimiter.size();
        if (body.substr(offset).starts_with("--")) {
            offset += 2;
            if (offset != body.size() && body.substr(offset) != "\r\n") {
                return std::unexpected(MultipartError{"Unexpected data after multipart terminator"});
            }
            return form;
        }
        if (!body.substr(offset).starts_with("\r\n")) {
            return std::unexpected(MultipartError{"Multipart boundary must end with CRLF"});
        }
        offset += 2;
        if (++parts > limits.max_parts) {
            return std::unexpected(MultipartError{"Multipart part count exceeds configured limit"});
        }

        const auto header_end = body.find("\r\n\r\n", offset);
        if (header_end == std::string_view::npos || header_end - offset > limits.max_header_bytes) {
            return std::unexpected(MultipartError{"Multipart part headers are malformed or too large"});
        }
        std::optional<std::string> disposition;
        std::string content_type_value;
        auto headers = body.substr(offset, header_end - offset);
        while (!headers.empty()) {
            const auto line_end = headers.find("\r\n");
            const auto line = headers.substr(0, line_end);
            const auto colon = line.find(':');
            if (colon == std::string_view::npos || !valid_header_value(line)) {
                return std::unexpected(MultipartError{"Malformed multipart part header"});
            }
            const auto name = trim(line.substr(0, colon));
            const auto value = trim(line.substr(colon + 1));
            if (name == "Content-Disposition") disposition = std::string(value);
            if (name == "Content-Type") content_type_value = std::string(value);
            if (line_end == std::string_view::npos) break;
            headers.remove_prefix(line_end + 2);
        }
        if (!disposition || !disposition->starts_with("form-data")) {
            return std::unexpected(MultipartError{"Multipart part requires form-data Content-Disposition"});
        }
        const auto field_name = parameter(*disposition, "name");
        if (!field_name || field_name->empty()) {
            return std::unexpected(MultipartError{"Multipart part is missing a field name"});
        }
        const auto filename = parameter(*disposition, "filename");

        const auto content_begin = header_end + 4;
        const auto next = body.find(next_delimiter, content_begin);
        if (next == std::string_view::npos) {
            return std::unexpected(MultipartError{"Multipart part is missing a closing boundary"});
        }
        const auto content = body.substr(content_begin, next - content_begin);
        offset = next + 2;

        if (!filename) {
            if (content.size() > limits.max_field_bytes) {
                return std::unexpected(MultipartError{"Multipart field exceeds configured size limit"});
            }
            form.fields_[*field_name].emplace_back(content);
            continue;
        }

        UploadedFile file;
        file.field_name_ = *field_name;
        file.filename_ = *filename;
        file.content_type_ = std::move(content_type_value);
        file.size_ = content.size();
        if (content.size() <= limits.max_in_memory_file_bytes) {
            file.data_ = content;
        } else {
            auto temporary_file = spill_file(content, temporary_directory);
            if (!temporary_file) return std::unexpected(temporary_file.error());
            file.temporary_file_ = std::move(*temporary_file);
        }
        form.files_.push_back(std::move(file));
    }
}

} // namespace novaboot::http
