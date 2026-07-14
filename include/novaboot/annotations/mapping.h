#pragma once

namespace novaboot::annotations {

#define NOVABOOT_DECLARE_MAPPING(Name) \
struct Name { \
    char path[64] = {}; \
    consteval Name() = default; \
    consteval explicit Name(const char* p) { \
        int i = 0; \
        while (p[i] && i < 63) { \
            path[i] = p[i]; \
            i++; \
        } \
        path[i] = '\0'; \
    } \
}

/// Mark a controller method as mapping a GET request.
NOVABOOT_DECLARE_MAPPING(GetMapping);

/// Mark a controller method as mapping a POST request.
NOVABOOT_DECLARE_MAPPING(PostMapping);

/// Mark a controller method as mapping a PUT request.
NOVABOOT_DECLARE_MAPPING(PutMapping);

/// Mark a controller method as mapping a DELETE request.
NOVABOOT_DECLARE_MAPPING(DeleteMapping);

/// Mark a controller method as mapping a PATCH request.
NOVABOOT_DECLARE_MAPPING(PatchMapping);

/// Mark a controller method as mapping a HEAD request.
NOVABOOT_DECLARE_MAPPING(HeadMapping);

/// Mark a controller method as mapping an OPTIONS request.
NOVABOOT_DECLARE_MAPPING(OptionsMapping);

#undef NOVABOOT_DECLARE_MAPPING

} // namespace novaboot::annotations
