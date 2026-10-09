#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coreutils {
    std::string strip_0x(const std::string& s);
    std::vector<uint8_t> hex_to_bytes(const std::string& hex);
    std::string bytes_to_hex(const std::vector<uint8_t>& bytes);
    std::string bytes_to_hex(const uint8_t* data, size_t length);
}