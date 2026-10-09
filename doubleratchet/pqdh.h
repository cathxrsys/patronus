#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "corecrypto.h"
#include "crypto.h"
#include "pq.h"

namespace pqdh {

struct KeyPairs {
  std::vector<uint8_t> pq_pk;
  std::vector<uint8_t> pq_sk;
  std::vector<uint8_t> dh_pub;
  std::vector<uint8_t> dh_priv;
};

struct SharedSecrets {
  std::vector<uint8_t> pq_shared_secret;
  std::vector<uint8_t> dh_shared_secret;
};

KeyPairs generate_keypairs();
SharedSecrets derive_shared_secrets(const std::vector<uint8_t>& pq_ciphertext,
                                    const std::vector<uint8_t>& pq_sk,
                                    const std::vector<uint8_t>& dh_my_priv,
                                    const std::vector<uint8_t>& dh_their_pub);
pq::Encapsulated make_encapsulated(const std::vector<uint8_t>& pq_pk);
std::vector<uint8_t> make_shared_key(
    const std::vector<uint8_t>& pq_shared_secret,
    const std::vector<uint8_t>& dh_shared_secret);

}  // namespace pqdh