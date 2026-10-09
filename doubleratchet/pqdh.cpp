#include "pqdh.h"

namespace pqdh {

KeyPairs generate_keypairs() {
  KeyPairs keypairs;

  pq::KeyPair pq_keypair = pq::kem_generate_keypair();
  keypairs.pq_pk = pq_keypair.pk;
  keypairs.pq_sk = pq_keypair.sk;

  CoreCrypto::DHKeyPair dh_keypair = CoreCrypto::generate_dh_keypair();
  keypairs.dh_pub = dh_keypair.pubkey;
  keypairs.dh_priv = dh_keypair.privkey;

  CoreCrypto::secure_memory_zero(pq_keypair.sk);
  CoreCrypto::secure_memory_zero(dh_keypair.privkey);

  return keypairs;
}

SharedSecrets derive_shared_secrets(const std::vector<uint8_t>& pq_ciphertext,
                                    const std::vector<uint8_t>& pq_sk,
                                    const std::vector<uint8_t>& dh_my_priv,
                                    const std::vector<uint8_t>& dh_their_pub) {
  SharedSecrets secrets;

  secrets.pq_shared_secret = pq::kem_decapsulate(pq_ciphertext, pq_sk);

  auto dh_secret = CoreCrypto::derive_dh_secret(dh_my_priv, dh_their_pub);
  secrets.dh_shared_secret =
      std::vector<uint8_t>(dh_secret.begin(), dh_secret.end());

  CoreCrypto::secure_memory_zero(dh_secret.data(), dh_secret.size());

  return secrets;
}

pq::Encapsulated make_encapsulated(const std::vector<uint8_t>& pq_pk) {
  return pq::kem_encapsulate(pq_pk);
}

std::vector<uint8_t> make_shared_key(
    const std::vector<uint8_t>& pq_shared_secret,
    const std::vector<uint8_t>& dh_shared_secret) {
  std::vector<uint8_t> ikm;
  ikm.reserve(pq_shared_secret.size() + dh_shared_secret.size());
  ikm.insert(ikm.end(), pq_shared_secret.begin(), pq_shared_secret.end());
  ikm.insert(ikm.end(), dh_shared_secret.begin(), dh_shared_secret.end());

  const std::string salt_str = "DoubleRatchet|PQDH|v1.0|salt";
  std::vector<uint8_t> salt(salt_str.begin(), salt_str.end());

  const std::string info_str = "DoubleRatchet|PQDH|v1.0|info";
  std::vector<uint8_t> info(info_str.begin(), info_str.end());

  // hkdf_sha256's signature is (salt, ikm, info, length) — salt must go
  // first. HKDF-Extract's guarantee (turning a possibly non-uniform secret,
  // e.g. a raw X25519/KEM output, into a uniform PRK) specifically relies on
  // the salt occupying HMAC's key position and the secret occupying the
  // message position; swapped, the extraction step loses that guarantee.
  size_t output_key_len = 32;
  std::vector<uint8_t> shared_key = crypto::hkdf_sha256(
      std::span<const uint8_t>(salt.data(), salt.size()),
      std::span<const uint8_t>(ikm.data(), ikm.size()),
      std::span<const uint8_t>(info.data(), info.size()), output_key_len);

  CoreCrypto::secure_memory_zero(ikm);
  CoreCrypto::secure_memory_zero(salt);
  CoreCrypto::secure_memory_zero(info);

  return shared_key;
}

}  // namespace pqdh