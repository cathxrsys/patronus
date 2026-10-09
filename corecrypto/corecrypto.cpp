//
// Created by r on 13.10.2025.
//

#include <cstring>
#include "corecrypto.h"

std::atomic<int> CoreCrypto::instance_count{0};

CoreCrypto::CoreCrypto() {
  spdlog::info("init...");

  static bool initialized = [] {
    if (sodium_init() < 0) {
      spdlog::error("libsodium init failed");
      throw std::runtime_error("libsodium init failed");
    } else {
      spdlog::debug("libsodium init success");
    }
    return true;
  }();
  (void)initialized;
  ++instance_count;

  spdlog::info("init successful");
}

CoreCrypto::~CoreCrypto() {}

std::vector<uint8_t> CoreCrypto::random(size_t key_size) {
  std::vector<uint8_t> key(key_size);
  randombytes_buf(key.data(), key.size());
  return key;
}

string CoreCrypto::to_hex(const uint8_t* data, size_t length) {
  static const char hexd[] = "0123456789abcdef";
  std::string out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    out.push_back(hexd[data[i] >> 4]);
    out.push_back(hexd[data[i] & 0x0F]);
  }
  return out;
}

string CoreCrypto::to_hex(const std::vector<uint8_t>& data) {
  return to_hex(data.data(), data.size());
}

std::vector<uint8_t> CoreCrypto::from_hex(const string& hexstr) {
  std::string hex = hexstr;
  if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
    hex = hex.substr(2);
  }
  if (hex.length() % 2 != 0) {
    throw std::invalid_argument("Hex string must have an even length");
  }
  std::vector<uint8_t> data;
  data.reserve(hex.length() / 2);
  for (size_t i = 0; i < hex.length(); i += 2) {
    uint8_t byte =
        static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
    data.push_back(byte);
  }
  return data;
}

std::vector<uint8_t> CoreCrypto::encrypt(
    const std::vector<uint8_t>& plaintext, const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& nonce,
    const std::vector<uint8_t>& associated_data, bool exceptions) {
  if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
    if (exceptions)
      throw std::runtime_error("Invalid key size");
    else
      return std::vector<uint8_t>{};
  }
  if (nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES) {
    if (exceptions)
      throw std::runtime_error("Invalid nonce size");
    else
      return std::vector<uint8_t>{};
  }

  std::vector<uint8_t> ciphertext(plaintext.size() +
                                  crypto_aead_xchacha20poly1305_ietf_ABYTES);
  unsigned long long ciphertext_len = 0;

  if (crypto_aead_xchacha20poly1305_ietf_encrypt(
          ciphertext.data(), &ciphertext_len, plaintext.data(),
          plaintext.size(), associated_data.data(), associated_data.size(),
          nullptr, nonce.data(), key.data()) != 0) {
    if (exceptions)
      throw std::runtime_error("encryption failed");
    else
      return std::vector<uint8_t>{};
  }
  ciphertext.resize(ciphertext_len);

  return ciphertext;
}

std::vector<uint8_t> CoreCrypto::decrypt(
    const std::vector<uint8_t>& ciphertext, const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& nonce,
    const std::vector<uint8_t>& associated_data, bool exceptions) {
  if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
    if (exceptions)
      throw std::runtime_error("Invalid key size");
    else
      return std::vector<uint8_t>{};
  }
  if (nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES) {
    if (exceptions)
      throw std::runtime_error("Invalid nonce size");
    else
      return std::vector<uint8_t>{};
  }

  if (ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES)
    if (exceptions)
      throw std::runtime_error("ciphertext too short");
    else
      return std::vector<uint8_t>{};

  std::vector<uint8_t> decrypted(ciphertext.size() -
                                 crypto_aead_xchacha20poly1305_ietf_ABYTES);
  unsigned long long decrypted_len = 0;

  if (crypto_aead_xchacha20poly1305_ietf_decrypt(
          decrypted.data(), &decrypted_len, nullptr, ciphertext.data(),
          ciphertext.size(), associated_data.data(), associated_data.size(),
          nonce.data(), key.data()) != 0) {
    if (exceptions)
      throw std::runtime_error("decryption failed");
    else
      return std::vector<uint8_t>{};
  }
  decrypted.resize(decrypted_len);

  return decrypted;
}

void CoreCrypto::secure_memory_zero(std::vector<uint8_t>& buf) {
  sodium_memzero(buf.data(), buf.size());
}

void CoreCrypto::secure_memory_zero(void* const pnt, const size_t len) {
  sodium_memzero(pnt, len);
}

void CoreCrypto::secure_memory_zero(std::string& buf) {
  volatile char* p = const_cast<volatile char*>(buf.data());
  for (size_t i = 0; i < buf.size(); ++i) p[i] = '\0';
  buf.clear();
}

CoreCrypto::DHKeyPair CoreCrypto::generate_dh_keypair() {
  DHKeyPair kp;
  kp.pubkey.resize(DH_PUBKEY_SIZE);
  kp.privkey.resize(DH_PRIVKEY_SIZE);
  crypto_kx_keypair(kp.pubkey.data(), kp.privkey.data());
  return kp;
}

CoreCrypto::SessionKeys CoreCrypto::derive_dh_keys(
    bool is_client, const std::vector<uint8_t>& my_pk,
    const std::vector<uint8_t>& my_sk, const std::vector<uint8_t>& their_pk) {
  std::vector<uint8_t> rx(DH_SHARED_SIZE), tx(DH_SHARED_SIZE);
  int rc;
  if (is_client) {
    rc = crypto_kx_client_session_keys(rx.data(), tx.data(), my_pk.data(),
                                       my_sk.data(), their_pk.data());
  } else {
    rc = crypto_kx_server_session_keys(rx.data(), tx.data(), my_pk.data(),
                                       my_sk.data(), their_pk.data());
  }
  if (rc != 0) throw std::runtime_error("Session key derivation failed");

  return {rx, tx};
}

std::array<uint8_t, 32> CoreCrypto::derive_dh_secret(
    const std::vector<uint8_t>& my_sk, const std::vector<uint8_t>& their_pk) {
  if (my_sk.size() != 32 || their_pk.size() != 32) {
    throw std::runtime_error(
        "x25519_shared_secret: key sizes must be 32 bytes");
  }
  std::array<uint8_t, 32> shared;

  if (crypto_scalarmult(shared.data(), my_sk.data(), their_pk.data()) != 0) {
    throw std::runtime_error("x25519_shared_secret failed");
  }
  return shared;  // 32-byte symmetric DH secret
}

string CoreCrypto::generate_mnemonicphrase() {
  uint8_t entropy[32];
  randombytes_buf(entropy, sizeof(entropy));

  char mnemonic[256] = {0};
  if (!mnemonic_from_data(entropy, sizeof(entropy), mnemonic)) {
    throw std::runtime_error("Failed to generate mnemonic");
  }

  sodium_memzero(entropy, sizeof(entropy));
  string result(mnemonic);
  sodium_memzero(mnemonic, sizeof(mnemonic));

  return result;
}

bool CoreCrypto::mnemonicphrase_check(const string& mnemonic) {
  return mnemonic_check(mnemonic.c_str()) == 1;
}

std::vector<uint8_t> CoreCrypto::mnemonicphrase_to_seed(
    const string& mnemonic, const string& passphrase, size_t length) {
  uint8_t seed[64];

  if (length > 64) {
    throw std::invalid_argument(
        "Length of seed must be lower or equality than 64");
  }

  mnemonic_to_seed(mnemonic.c_str(), passphrase.c_str(), seed, nullptr);

  std::vector<uint8_t> result(seed, seed + length);

  sodium_memzero(seed, sizeof(seed));

  return result;
}

CoreCrypto::SignKeyPair CoreCrypto::seed_to_keypair(
    const std::vector<uint8_t>& seed) {
  if (seed.size() != crypto_sign_SEEDBYTES)  // 32 bytes
    throw std::invalid_argument("Seed must be 32 bytes for Ed25519!");

  std::vector<uint8_t> pubkey(crypto_sign_PUBLICKEYBYTES);
  std::vector<uint8_t> privkey(crypto_sign_SECRETKEYBYTES);

  if (crypto_sign_seed_keypair(pubkey.data(), privkey.data(), seed.data()) != 0)
    throw std::runtime_error("Keypair generation failed");

  return {privkey, pubkey};
}

string CoreCrypto::fingerprint(const std::vector<uint8_t>& vec) {
  uint8_t hash[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(hash, vec.data(), vec.size());

  return "0x" + to_hex(hash, sizeof(hash));
}

std::vector<uint8_t> CoreCrypto::sha256(const std::vector<uint8_t>& vec) {
  uint8_t hash[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(hash, vec.data(), vec.size());
  std::vector<uint8_t> result(hash, hash + crypto_hash_sha256_BYTES);
  CoreCrypto::secure_memory_zero(hash, sizeof(hash));
  return result;
}

std::vector<uint8_t> CoreCrypto::sign(const std::vector<uint8_t>& privkey,
                                      const std::vector<uint8_t>& message) {
  if (privkey.size() != crypto_sign_SECRETKEYBYTES)
    throw std::invalid_argument("Invalid privkey size for Ed25519!");

  std::vector<uint8_t> signature(crypto_sign_BYTES);  // 64 bytes
  unsigned long long siglen = 0;

  if (crypto_sign_detached(signature.data(), &siglen, message.data(),
                           message.size(), privkey.data()) != 0)
    throw std::runtime_error("Signing failed");

  signature.resize(siglen);  // usually 64, but keep the exact size
  return signature;
}

bool CoreCrypto::verify(const std::vector<uint8_t>& pubkey,
                        const std::vector<uint8_t>& message,
                        const std::vector<uint8_t>& signature) {
  if (pubkey.size() != crypto_sign_PUBLICKEYBYTES)
    throw std::invalid_argument("Invalid pubkey size for Ed25519!");
  if (signature.size() != crypto_sign_BYTES) return false;
  return crypto_sign_verify_detached(signature.data(), message.data(),
                                     message.size(), pubkey.data()) == 0;
}

std::string CoreCrypto::to_base64(const unsigned char* data, size_t len,
                                  int variant) {
  // compute the maximum encoded string length (including '\0')
  size_t b64_maxlen = sodium_base64_encoded_len(len, variant);
  std::string out;
  out.resize(b64_maxlen);  // allocate space
  // sodium_bin2base64 writes a null-terminated string
  sodium_bin2base64(&out[0], b64_maxlen, data, len, variant);
  out.resize(
      std::strlen(out.c_str()));  // trim to the actual length (without '\0')
  return out;
}

bool CoreCrypto::from_base64(const std::string& b64,
                             std::vector<unsigned char>& out, int variant) {
  // allocate the largest possible buffer (base64 length gives an upper bound)
  out.resize(b64.size());
  size_t bin_len = 0;
  // the ignore argument can be nullptr or a string of characters to ignore
  // (for example "\n")
  if (sodium_base642bin(out.data(), out.size(), b64.c_str(), b64.size(),
                        nullptr, &bin_len, nullptr, variant) != 0) {
    return false;  // decoding error
  }
  out.resize(bin_len);
  return true;
}