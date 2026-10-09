#include "coreutils.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <iomanip>


namespace coreutils {

    std::string strip_0x(const std::string& s) {
        if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            return s.substr(2);
        return s;
    }

    std::vector<uint8_t> hex_to_bytes(const std::string& hex) {

        std::string clean_hex = hex;
        // Remove leading "\x" if present
        if (clean_hex.size() >= 2 && (clean_hex[0] == '0' || clean_hex[0] == '\\') && (clean_hex[1] == 'x' || clean_hex[1] == 'X')) {
            clean_hex = clean_hex.substr(2);
        }

        if (clean_hex.size() % 2 != 0)
            throw std::invalid_argument("Hex string must have even length");

        std::vector<uint8_t> out;
        //out.reserve(32);
        for (size_t i = 0; i < clean_hex.size(); i += 2) {
            std::string byte_str = clean_hex.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
            out.push_back(byte);
        }
        return out;
    }

    std::string bytes_to_hex(const std::vector<uint8_t>& bytes) {
        static const char hex_digits[] = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (uint8_t b : bytes) {
            out.push_back(hex_digits[b >> 4]);
            out.push_back(hex_digits[b & 0x0F]);
        }
        return out;
    }

    std::string bytes_to_hex(const uint8_t* data, size_t length) {
        std::ostringstream oss;
        for (size_t i = 0; i < length; i++) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
        }
        return oss.str();
    }
}
