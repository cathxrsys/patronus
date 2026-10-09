#include "pq.h"

namespace pq {

KeyPair kem_generate_keypair() {
  OQS_KEM* kem = OQS_KEM_new(MLKEM_ALG);
  if (!kem) throw std::runtime_error("OQS_KEM_new failed");

  KeyPair keypair;
  keypair.pk.resize(kem->length_public_key);
  keypair.sk.resize(kem->length_secret_key);

  if (OQS_KEM_keypair(kem, keypair.pk.data(), keypair.sk.data()) !=
      OQS_SUCCESS) {
    OQS_KEM_free(kem);
    throw std::runtime_error("OQS_KEM_keypair failed");
  }

  OQS_KEM_free(kem);
  return keypair;
}

Encapsulated kem_encapsulate(const std::vector<uint8_t>& pk) {
  OQS_KEM* kem = OQS_KEM_new(MLKEM_ALG);
  if (!kem) throw std::runtime_error("OQS_KEM_new failed");

  // OQS_KEM_encaps has no way to know how many bytes pk actually points to —
  // it unconditionally reads length_public_key bytes. A caller-supplied
  // buffer shorter than that (e.g. a malicious peer's undersized prekey
  // field) would be a heap buffer over-read, not a clean error.
  if (pk.size() != kem->length_public_key) {
    OQS_KEM_free(kem);
    throw std::runtime_error("kem_encapsulate: invalid public key length");
  }

  Encapsulated result;
  result.ciphertext.resize(kem->length_ciphertext);
  result.shared_secret.resize(kem->length_shared_secret);

  if (OQS_KEM_encaps(kem, result.ciphertext.data(), result.shared_secret.data(),
                     pk.data()) != OQS_SUCCESS) {
    OQS_KEM_free(kem);
    throw std::runtime_error("OQS_KEM_encaps failed");
  }

  OQS_KEM_free(kem);
  return result;
}

std::vector<uint8_t> kem_decapsulate(const std::vector<uint8_t>& ciphertext,
                                     const std::vector<uint8_t>& sk) {
  OQS_KEM* kem = OQS_KEM_new(MLKEM_ALG);
  if (!kem) throw std::runtime_error("OQS_KEM_new failed");

  // Same reasoning as kem_encapsulate: OQS_KEM_decaps trusts the caller for
  // both lengths and reads them unconditionally. ciphertext in particular
  // comes straight off the wire (an incoming session_request), so an
  // attacker fully controls its length.
  if (ciphertext.size() != kem->length_ciphertext) {
    OQS_KEM_free(kem);
    throw std::runtime_error("kem_decapsulate: invalid ciphertext length");
  }
  if (sk.size() != kem->length_secret_key) {
    OQS_KEM_free(kem);
    throw std::runtime_error("kem_decapsulate: invalid secret key length");
  }

  std::vector<uint8_t> shared_secret(kem->length_shared_secret);

  if (OQS_KEM_decaps(kem, shared_secret.data(), ciphertext.data(), sk.data()) !=
      OQS_SUCCESS) {
    OQS_KEM_free(kem);
    throw std::runtime_error("OQS_KEM_decaps failed");
  }

  OQS_KEM_free(kem);
  return shared_secret;
}

}  // namespace pq