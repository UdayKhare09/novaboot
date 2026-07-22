#pragma once

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "novaboot/router/router.h"
#include "novaboot/validation/validation.h"

namespace novaboot::openapi {

/// Produces a deterministic OpenAPI 3.1 document from NovaBoot's registered
/// routes. It intentionally describes only public router metadata: fluent and
/// reflected controller routes have identical output.
class Document {
public:
    struct Config {
        std::string title = "NovaBoot application";
        std::string version = "1.0.0";
        std::string description;
        std::vector<std::string> servers;
    };

    explicit Document(const router::Router& router)
        : Document(router, Config{}) {}

    explicit Document(const router::Router& router, Config config)
        : config_(std::move(config)) {
        for (const auto& route : router.routes()) {
            const auto method = router::method_to_string(route.method);
            if (method == "*") continue; // OpenAPI does not define an ANY verb.
            Operation operation;
            operation.source_pattern = route.pattern;
            operations_[openapi_path(route.pattern)][std::string(method)] = std::move(operation);
        }
    }

    /// Register a JSON Schema generated from a NovaBoot validation schema.
    /// The router stores type-erased handlers, so request/response schemas are
    /// attached explicitly below rather than guessed from a callback.
    template<typename T>
    Document& schema(std::string name, const validation::Schema<T>& validation) {
        schemas_.insert_or_assign(std::move(name), schema_json(validation.fields()));
        return *this;
    }

    /// Attach a registered JSON request schema to a route operation.
    /// `path` uses the route syntax (`:id`) or OpenAPI syntax (`{id}`).
    Document& request_body(std::string_view path, router::Method method,
                           std::string schema_name) {
        const auto method_name = router::method_to_string(method);
        const auto path_it = operations_.find(openapi_path(path));
        if (path_it == operations_.end() || method_name == "*" ||
            !path_it->second.contains(std::string(method_name))) {
            throw std::invalid_argument("OpenAPI request body refers to an unregistered route operation");
        }
        if (!schemas_.contains(schema_name)) {
            throw std::invalid_argument("OpenAPI request body refers to an unknown schema");
        }
        path_it->second.at(std::string(method_name)).request_schema = std::move(schema_name);
        return *this;
    }

    [[nodiscard]] std::string json() const {
        std::string output = R"({"openapi":"3.1.0","info":{"title":")" +
            quote(config_.title) + R"(","version":")" + quote(config_.version) + '"';
        if (!config_.description.empty()) {
            output += R"(,"description":")" + quote(config_.description) + '"';
        }
        output += "}";
        if (!config_.servers.empty()) {
            output += R"(,"servers":[)";
            bool first = true;
            for (const auto& server : config_.servers) {
                if (!first) output += ',';
                first = false;
                output += R"({"url":")" + quote(server) + R"("})";
            }
            output += ']';
        }
        output += R"(,"paths":{)";
        bool first_path = true;
        for (const auto& [path, methods] : operations_) {
            if (!first_path) output += ',';
            first_path = false;
            output += '"' + quote(path) + R"(":{)";
            bool first_method = true;
            for (const auto& [method, operation] : methods) {
                if (!first_method) output += ',';
                first_method = false;
                output += '"' + lowercase(method) + R"(":{)";
                output += R"("operationId":")" + quote(operation_id(method, operation.source_pattern)) + '"';
                const auto parameters = path_parameters(operation.source_pattern);
                if (!parameters.empty()) {
                    output += R"(,"parameters":[)";
                    for (std::size_t index = 0; index < parameters.size(); ++index) {
                        if (index != 0) output += ',';
                        output += R"({"name":")" + quote(parameters[index]) +
                            R"(","in":"path","required":true,"schema":{"type":"string"}})";
                    }
                    output += ']';
                }
                if (operation.request_schema) {
                    output += R"(,"requestBody":{"required":true,"content":{"application/json":{"schema":{"$ref":"#/components/schemas/)" +
                        quote(*operation.request_schema) + R"("}}}})";
                }
                output += R"(,"responses":{"200":{"description":"Successful response"}}})";
            }
            output += '}';
        }
        output += "}";
        if (!schemas_.empty()) {
            output += R"(,"components":{"schemas":{)";
            bool first_schema = true;
            for (const auto& [name, schema] : schemas_) {
                if (!first_schema) output += ',';
                first_schema = false;
                output += '"' + quote(name) + R"(":)" + schema;
            }
            output += "}}";
        }
        return output + "}";
    }

private:
    Config config_;
    struct Operation {
        std::string source_pattern;
        std::optional<std::string> request_schema;
    };

    std::map<std::string, std::map<std::string, Operation>> operations_;
    std::map<std::string, std::string> schemas_;

    template<typename Field>
    static std::string schema_json(const std::vector<Field>& fields) {
        std::string output = R"({"type":"object","properties":{)";
        bool first = true;
        std::vector<std::string_view> required;
        for (const auto& field : fields) {
            if (!first) output += ',';
            first = false;
            output += '"' + quote(field.name) + R"(":{"type":")" + quote(field.json_type) + '"';
            if (field.item_json_type) {
                output += R"(,"items":{"type":")" + quote(*field.item_json_type) + R"("})";
            }
            if (field.minimum) output += R"(,"minimum":)" + std::to_string(*field.minimum);
            if (field.maximum) output += R"(,"maximum":)" + std::to_string(*field.maximum);
            if (field.min_length) output += R"(,"minLength":)" + std::to_string(*field.min_length);
            if (field.max_length) output += R"(,"maxLength":)" + std::to_string(*field.max_length);
            if (field.format) output += R"(,"format":")" + quote(*field.format) + '"';
            output += '}';
            if (field.required) required.push_back(field.name);
        }
        output += '}';
        if (!required.empty()) {
            output += R"(,"required":[)";
            for (std::size_t index = 0; index < required.size(); ++index) {
                if (index) output += ',';
                output += '"' + quote(required[index]) + '"';
            }
            output += ']';
        }
        return output + '}';
    }

    [[nodiscard]] static std::string quote(std::string_view value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (const unsigned char character : value) {
            switch (character) {
            case '"': escaped += R"(\")"; break;
            case '\\': escaped += R"(\\)"; break;
            case '\b': escaped += R"(\b)"; break;
            case '\f': escaped += R"(\f)"; break;
            case '\n': escaped += R"(\n)"; break;
            case '\r': escaped += R"(\r)"; break;
            case '\t': escaped += R"(\t)"; break;
            default:
                if (character < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped.push_back(hex[character >> 4]);
                    escaped.push_back(hex[character & 0x0f]);
                } else {
                    escaped.push_back(static_cast<char>(character));
                }
            }
        }
        return escaped;
    }

    [[nodiscard]] static std::string lowercase(std::string value) {
        for (auto& character : value) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        return value;
    }

    [[nodiscard]] static std::string openapi_path(std::string_view pattern) {
        std::string path;
        std::size_t offset = 0;
        while (offset < pattern.size()) {
            const auto slash = pattern.find('/', offset);
            const auto end = slash == std::string_view::npos ? pattern.size() : slash;
            const auto segment = pattern.substr(offset, end - offset);
            if (!segment.empty() && (segment.front() == ':' || segment.front() == '*')) {
                path += '{';
                path.append(segment.substr(1));
                path += '}';
            } else {
                path.append(segment);
            }
            if (slash == std::string_view::npos) break;
            path += '/';
            offset = slash + 1;
        }
        return path.empty() ? "/" : path;
    }

    [[nodiscard]] static std::vector<std::string> path_parameters(std::string_view pattern) {
        std::vector<std::string> parameters;
        std::size_t offset = 0;
        while (offset < pattern.size()) {
            const auto slash = pattern.find('/', offset);
            const auto end = slash == std::string_view::npos ? pattern.size() : slash;
            const auto segment = pattern.substr(offset, end - offset);
            if (segment.size() > 1 && (segment.front() == ':' || segment.front() == '*')) {
                parameters.emplace_back(segment.substr(1));
            }
            if (slash == std::string_view::npos) break;
            offset = slash + 1;
        }
        return parameters;
    }

    [[nodiscard]] static std::string operation_id(std::string_view method,
                                                   std::string_view pattern) {
        std::string id = lowercase(std::string(method));
        for (const auto character : pattern) {
            if ((character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9')) {
                id.push_back(character);
            } else {
                id.push_back('_');
            }
        }
        return id;
    }
};

/// Add a JSON endpoint that serves a snapshot of the registered routes.
inline void serve(router::Router& router, std::string_view path,
                  Document::Config config) {
    auto document = std::make_shared<Document>(router, std::move(config));
    router.add_route(router::Method::GET, path,
        [document = std::move(document)](http3::Request&, http3::Response& response,
                                         context::RequestContext&) {
            response.status(200).header("content-type", "application/vnd.oai.openapi+json;version=3.1")
                .body(document->json());
        });
}

inline void serve(router::Router& router, std::string_view path = "/openapi.json") {
    serve(router, path, Document::Config{});
}

} // namespace novaboot::openapi
