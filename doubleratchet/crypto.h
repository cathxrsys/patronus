#pragma once

#include <vector>
#include <span>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace crypto {

/// HMAC-SHA256(key, data) -> 32 bytes
    std::vector<uint8_t> hmac_sha256(std::span<const uint8_t> key,
                                     std::span<const uint8_t> data);

/// HKDF-Extract (RFC 5869): PRK = HMAC(salt (or zeros), IKM)
    std::vector<uint8_t> hkdf_extract(std::span<const uint8_t> salt,
                                      std::span<const uint8_t> ikm);

/// HKDF-Expand (RFC 5869)
    std::vector<uint8_t> hkdf_expand(std::span<const uint8_t> prk,
                                     std::span<const uint8_t> info,
                                     size_t length);

/// Full HKDF
    std::vector<uint8_t> hkdf_sha256(std::span<const uint8_t> salt,
                                     std::span<const uint8_t> ikm,
                                     std::span<const uint8_t> info,
                                     size_t length);

}