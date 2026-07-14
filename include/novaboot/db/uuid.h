#pragma once
#include <array>
#include <string>
#include <string_view>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <cstdlib>

namespace novaboot::db {

class Uuid {
public:
    std::array<std::uint8_t, 16> data{};

    Uuid() = default;

    /// Generate a strictly time-ordered UUIDv7 (RFC 9562)
    static Uuid generate() {
        Uuid uuid{};
        
        // 1. 48-bit Unix timestamp in milliseconds
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        
        uuid.data[0] = static_cast<std::uint8_t>((millis >> 40) & 0xFF);
        uuid.data[1] = static_cast<std::uint8_t>((millis >> 32) & 0xFF);
        uuid.data[2] = static_cast<std::uint8_t>((millis >> 24) & 0xFF);
        uuid.data[3] = static_cast<std::uint8_t>((millis >> 16) & 0xFF);
        uuid.data[4] = static_cast<std::uint8_t>((millis >> 8) & 0xFF);
        uuid.data[5] = static_cast<std::uint8_t>(millis & 0xFF);
        
        // 2. Fill the remaining 10 bytes with entropy
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dis(0, 255);
        for (int i = 6; i < 16; ++i) {
            uuid.data[i] = static_cast<std::uint8_t>(dis(gen));
        }
        
        // 3. Set version to 7 (bits 4-7 of byte 6 = 0111 = 0x70)
        uuid.data[6] = (uuid.data[6] & 0x0F) | 0x70;
        
        // 4. Set variant to 2 (RFC 4122) (bits 6-7 of byte 8 = 10 = 0x80)
        uuid.data[8] = (uuid.data[8] & 0x3F) | 0x80;
        
        return uuid;
    }

    bool is_nil() const {
        for (auto b : data) {
            if (b != 0) return false;
        }
        return true;
    }

    std::string to_string() const {
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < 16; ++i) {
            ss << std::setw(2) << static_cast<int>(data[i]);
            if (i == 3 || i == 5 || i == 7 || i == 9) {
                ss << "-";
            }
        }
        return ss.str();
    }

    static Uuid from_string(std::string_view sv) {
        Uuid uuid{};
        size_t idx = 0;
        for (size_t i = 0; i < sv.length() && idx < 16; ++i) {
            if (sv[i] == '-') continue;
            if (i + 1 < sv.length()) {
                char hex[3] = { sv[i], sv[i+1], '\0' };
                uuid.data[idx++] = static_cast<std::uint8_t>(std::strtol(hex, nullptr, 16));
                i++;
            }
        }
        return uuid;
    }

    bool operator==(const Uuid& other) const { return data == other.data; }
    bool operator!=(const Uuid& other) const { return data != other.data; }
    bool operator<(const Uuid& other) const { return data < other.data; }
};

} // namespace novaboot::db
