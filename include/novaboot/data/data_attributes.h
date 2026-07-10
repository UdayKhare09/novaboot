#pragma once

#ifndef ODB_COMPILER

#include "novaboot/di/attributes.h"
#include <cstdint>
#include <functional>

namespace novaboot::data {

struct entity {
    char table_name[64] = {};

    consteval entity(const char* s) noexcept {
        for (std::size_t i = 0; i < 63u && s[i]; ++i) {
            table_name[i] = s[i];
        }
    }
};

struct id {};

struct column {
    char col_name[64] = {};

    consteval column(const char* s) noexcept {
        for (std::size_t i = 0; i < 63u && s[i]; ++i) {
            col_name[i] = s[i];
        }
    }
};

struct cacheable {
    uint32_t ttl_seconds = 60;
};

struct transient {};

struct version {};

} // namespace novaboot::data

#endif // ODB_COMPILER
