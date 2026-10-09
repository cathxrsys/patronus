#include "prekeys.h"

namespace prekeys {

PreKey to_prekey(const std::vector<uint8_t>& id,
                 const std::vector<uint8_t>& dh_pub,
                 const std::vector<uint8_t>& pq_pub,
                 const std::vector<uint8_t>& Ed25519_pub,
                 const std::vector<uint8_t>& signature) {
  PreKey pk;

  pk.id = id;
  pk.dh_pub = dh_pub;
  pk.pq_pub = pq_pub;
  pk.Ed25519_pub = Ed25519_pub;
  pk.signature = signature;
  return pk;
}

std::string prekey_to_json(const PreKey& prekey) {
  std::string result("{");
  result += "\"id\":\"" + coreutils::bytes_to_hex(prekey.id) + "\",";
  result += "\"dh_pub\":\"" + coreutils::bytes_to_hex(prekey.dh_pub) + "\",";
  result += "\"pq_pub\":\"" + coreutils::bytes_to_hex(prekey.pq_pub) + "\",";
  result += "\"ed25519_pub\":\"" + coreutils::bytes_to_hex(prekey.Ed25519_pub) +
            "\",";
  result +=
      "\"signature\":\"" + coreutils::bytes_to_hex(prekey.signature) + "\"}";
  return result;
}

PreKey json_to_prekey(const std::string& json_str) {
  nlohmann::json j = nlohmann::json::parse(json_str);
  PreKey pk;
  pk.id = coreutils::hex_to_bytes(j.at("id").get<std::string>());
  pk.dh_pub = coreutils::hex_to_bytes(j.at("dh_pub").get<std::string>());
  pk.pq_pub = coreutils::hex_to_bytes(j.at("pq_pub").get<std::string>());
  pk.Ed25519_pub =
      coreutils::hex_to_bytes(j.at("ed25519_pub").get<std::string>());
  pk.signature = coreutils::hex_to_bytes(j.at("signature").get<std::string>());
  return pk;
}

bool verify_prekey(const PreKey& prekey) {
  std::vector<uint8_t> signed_data;
  signed_data.reserve(prekey.id.size() + prekey.dh_pub.size() +
                      prekey.pq_pub.size() + prekey.Ed25519_pub.size());
  if (prekey.id.empty() || prekey.dh_pub.empty() || prekey.pq_pub.empty() ||
      prekey.Ed25519_pub.empty() || prekey.signature.empty()) {
    return false;
  }

  if (prekey.id.size() != 32) return false;
  if (prekey.dh_pub.size() != 32) return false;
  if (prekey.Ed25519_pub.size() != 32) return false;

  signed_data.insert(signed_data.end(), prekey.id.begin(), prekey.id.end());
  signed_data.insert(signed_data.end(), prekey.dh_pub.begin(),
                     prekey.dh_pub.end());
  signed_data.insert(signed_data.end(), prekey.pq_pub.begin(),
                     prekey.pq_pub.end());
  signed_data.insert(signed_data.end(), prekey.Ed25519_pub.begin(),
                     prekey.Ed25519_pub.end());

  return CoreCrypto::verify(prekey.Ed25519_pub, signed_data, prekey.signature);
}

}  // namespace prekeys