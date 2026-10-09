#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

extern "C" {
#include <oqs/oqs.h>
}

namespace pq {

static constexpr const char* MLKEM_ALG = OQS_KEM_alg_kyber_1024;

struct KeyPair {
  std::vector<uint8_t> pk;
  std::vector<uint8_t> sk;
};

struct Encapsulated {
  std::vector<uint8_t> ciphertext;
  std::vector<uint8_t> shared_secret;
};

KeyPair kem_generate_keypair();
Encapsulated kem_encapsulate(const std::vector<uint8_t>& pk);
std::vector<uint8_t> kem_decapsulate(const std::vector<uint8_t>& ciphertext,
                                     const std::vector<uint8_t>& sk);

}  // namespace pq