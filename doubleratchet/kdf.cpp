#include "kdf.h"

#include <cstring>
#include <vector>

#include "corecrypto.h"

namespace kdf {

void kdf_root(const Key32& previous_root_key, const Key32& dh_secret,
              Key32& new_root, Key32& new_chain_key_send,
              Key32& new_chain_key_receive) {
  // HKDF-Extract(previous_root_key, dh_secret) → PRK
  auto prk_vec = crypto::hkdf_extract(
      std::span<const uint8_t>(previous_root_key.data(),
                               previous_root_key.size()),
      std::span<const uint8_t>(dh_secret.data(), dh_secret.size()));
  if (prk_vec.size() != KEY_LEN) {
    throw std::runtime_error("hkdf_extract: unexpected PRK length");
  }

  // HKDF-Expand(PRK, "DoubleRatchetRoot|v1.0", 3*KEY_LEN)
  constexpr size_t OUT_LEN = KEY_LEN * 3;  // 96 bytes, for three keys
  static const char info_str[] = "DoubleRatchetRoot|v1.0";
  auto okm = crypto::hkdf_expand(
      std::span<const uint8_t>(prk_vec.data(), prk_vec.size()),
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(info_str),
                               sizeof(info_str) - 1),
      OUT_LEN);
  if (okm.size() != OUT_LEN) {
    throw std::runtime_error("hkdf_expand: unexpected OKM length");
  }

  std::memcpy(new_root.data(), okm.data() + 0 * KEY_LEN, KEY_LEN);
  std::memcpy(new_chain_key_send.data(), okm.data() + 1 * KEY_LEN, KEY_LEN);
  std::memcpy(new_chain_key_receive.data(), okm.data() + 2 * KEY_LEN, KEY_LEN);

  CoreCrypto::secure_memory_zero(prk_vec.data(), prk_vec.size());
  CoreCrypto::secure_memory_zero(okm.data(), okm.size());
}

static void kdf_chain_generic(Key32& chain_key, Key32& message_key) {
  static const char chain_info_str[] = "DoubleRatchetChain|v1.0";

  auto make_hmac_data = [](uint8_t label) {
    std::vector<uint8_t> data;
    data.insert(data.end(), chain_info_str,
                chain_info_str + sizeof(chain_info_str) - 1);
    data.push_back(label);
    return data;
  };

  auto next_chain_data = make_hmac_data(0x01);
  auto next_chain_vec = crypto::hmac_sha256(
      std::span<const uint8_t>(chain_key.data(), chain_key.size()),
      std::span<const uint8_t>(next_chain_data.data(), next_chain_data.size()));
  if (next_chain_vec.size() != KEY_LEN) {
    throw std::runtime_error("hmac_sha256: unexpected next_chain_key length");
  }

  auto message_data = make_hmac_data(0x02);
  auto message_vec = crypto::hmac_sha256(
      std::span<const uint8_t>(chain_key.data(), chain_key.size()),
      std::span<const uint8_t>(message_data.data(), message_data.size()));
  if (message_vec.size() != KEY_LEN) {
    throw std::runtime_error("hmac_sha256: unexpected message_key length");
  }

  std::memcpy(chain_key.data(), next_chain_vec.data(), KEY_LEN);
  std::memcpy(message_key.data(), message_vec.data(), KEY_LEN);

  CoreCrypto::secure_memory_zero(next_chain_vec.data(), next_chain_vec.size());
  CoreCrypto::secure_memory_zero(message_vec.data(), message_vec.size());
  std::fill(next_chain_data.begin(), next_chain_data.end(), 0);
  std::fill(message_data.begin(), message_data.end(), 0);
}

void kdf_chain_send(Key32& chain_key_send, Key32& message_key_send) {
  kdf_chain_generic(chain_key_send, message_key_send);
}

void kdf_chain_receive(Key32& chain_key_receive, Key32& message_key_receive) {
  kdf_chain_generic(chain_key_receive, message_key_receive);
}

}  // namespace kdf