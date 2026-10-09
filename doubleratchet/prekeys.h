#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

#include "corecrypto.h"
#include "coreutils.h"

namespace prekeys {
struct PreKey {
  std::vector<uint8_t> id;
  std::vector<uint8_t> dh_pub;
  std::vector<uint8_t> pq_pub;
  std::vector<uint8_t> Ed25519_pub;
  std::vector<uint8_t> signature;
};

PreKey to_prekey(const std::vector<uint8_t>& id,
                 const std::vector<uint8_t>& dh_pub,
                 const std::vector<uint8_t>& pq_pub,
                 const std::vector<uint8_t>& Ed25519_pub,
                 const std::vector<uint8_t>& signature);

std::string prekey_to_json(const PreKey& prekey);
PreKey json_to_prekey(const std::string& json_str);

bool verify_prekey(const PreKey& prekey);
}  // namespace prekeys