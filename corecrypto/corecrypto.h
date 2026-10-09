#pragma once

#include <sodium.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#include <array>

extern "C" {
#include "bip39.h"
}

extern "C" {
int crypto_scalarmult(unsigned char* q, const unsigned char* n,
                      const unsigned char* p);
}

using std::string;

class CoreCrypto {
 public:
  static constexpr size_t DH_PUBKEY_SIZE = crypto_kx_PUBLICKEYBYTES;
  static constexpr size_t DH_PRIVKEY_SIZE = crypto_kx_SECRETKEYBYTES;
  static constexpr size_t DH_SHARED_SIZE = crypto_kx_SESSIONKEYBYTES;
  static constexpr size_t ENCRYPT_NONCE_SIZE =
      crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
  static constexpr size_t SIGN_PUBKEY_SIZE = crypto_sign_PUBLICKEYBYTES;
  static constexpr size_t SIGN_PRIVKEY_SIZE = crypto_sign_SECRETKEYBYTES;
  static constexpr size_t SIGN_PUBKEY_FINGERPRINT_SIZE =
      20 + 2;  // "0x" + 20 bytes in hex

  struct DHKeyPair {
    std::vector<uint8_t> pubkey;
    std::vector<uint8_t> privkey;
  };

  struct SessionKeys {
    std::vector<uint8_t> receive_key;
    std::vector<uint8_t> send_key;
  };

  CoreCrypto();

  ~CoreCrypto();

  template <size_t N>
  static std::array<uint8_t, N> random() {
    std::array<uint8_t, N> arr;
    randombytes_buf(arr.data(), arr.size());
    return arr;
  }

  static std::vector<uint8_t> random(size_t key_size);

  static string to_hex(const std::vector<uint8_t>& data);

  static string to_hex(const uint8_t* data, size_t length);

  static std::vector<uint8_t> from_hex(const string& hexstr);

  static std::vector<uint8_t> encrypt(
      const std::vector<uint8_t>& plaintext, const std::vector<uint8_t>& key,
      const std::vector<uint8_t>& nonce,
      const std::vector<uint8_t>& associated_data, bool exceptions = true);

  static std::vector<uint8_t> decrypt(
      const std::vector<uint8_t>& ciphertext, const std::vector<uint8_t>& key,
      const std::vector<uint8_t>& nonce,
      const std::vector<uint8_t>& associated_data, bool exceptions = true);

  static void secure_memory_zero(std::vector<uint8_t>& buf);

  static void secure_memory_zero(void* const pnt, const size_t len);

  static void secure_memory_zero(std::string& buf);

  static DHKeyPair generate_dh_keypair();

  static SessionKeys derive_dh_keys(bool is_client,
                                    const std::vector<uint8_t>& my_pk,
                                    const std::vector<uint8_t>& my_sk,
                                    const std::vector<uint8_t>& their_pk);

  static std::array<uint8_t, 32> derive_dh_secret(
      const std::vector<uint8_t>& my_sk, const std::vector<uint8_t>& their_pk);

  static string generate_mnemonicphrase();

  static bool mnemonicphrase_check(const string& mnemonic);

  static std::vector<uint8_t> mnemonicphrase_to_seed(
      const string& mnemonic, const string& passphrase = "",
      size_t length = 32);

  struct SignKeyPair {
    std::vector<uint8_t> privkey;
    std::vector<uint8_t> pubkey;
  };

  static SignKeyPair seed_to_keypair(const std::vector<uint8_t>& seed);

  static string fingerprint(const std::vector<uint8_t>& vec);

  static std::vector<uint8_t> sha256(const std::vector<uint8_t>& vec);

  static std::vector<uint8_t> sign(const std::vector<uint8_t>& privkey,
                                   const std::vector<uint8_t>& message);

  static bool verify(const std::vector<uint8_t>& pubkey,
                     const std::vector<uint8_t>& message,
                     const std::vector<uint8_t>& signature);

  static std::string to_base64(
      const unsigned char* data, size_t len,
      int variant = sodium_base64_VARIANT_URLSAFE_NO_PADDING);

  static bool from_base64(
      const std::string& b64, std::vector<unsigned char>& out,
      int variant = sodium_base64_VARIANT_URLSAFE_NO_PADDING);

 private:
  static std::atomic<int> instance_count;
};