#include "crypto.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>

#include <array>
#include <iomanip>
#include <sstream>

namespace crypto {

namespace {
constexpr size_t SHA256_LEN = 32;

std::vector<uint8_t> hmac_sha256_impl(
    std::span<const uint8_t> key,
    const std::vector<std::span<const uint8_t>>& parts) {
  std::vector<uint8_t> out(SHA256_LEN);
  size_t out_len = 0;

  EVP_MAC* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  if (!mac) {
    throw std::runtime_error("EVP_MAC_fetch(HMAC) failed");
  }

  EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
  if (!ctx) {
    EVP_MAC_free(mac);
    throw std::runtime_error("EVP_MAC_CTX_new failed");
  }

  const char* digest = "SHA256";
  OSSL_PARAM params[] = {
      OSSL_PARAM_construct_utf8_string(OSSL_ALG_PARAM_DIGEST,
                                       const_cast<char*>(digest), 0),
      OSSL_PARAM_construct_end()};

  if (EVP_MAC_init(ctx, key.data(), key.size(), params) != 1) {
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
    throw std::runtime_error("EVP_MAC_init failed");
  }

  for (auto part : parts) {
    if (part.empty()) continue;
    if (EVP_MAC_update(ctx, part.data(), part.size()) != 1) {
      EVP_MAC_CTX_free(ctx);
      EVP_MAC_free(mac);
      throw std::runtime_error("EVP_MAC_update failed");
    }
  }

  if (EVP_MAC_final(ctx, out.data(), &out_len, out.size()) != 1) {
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
    throw std::runtime_error("EVP_MAC_final failed");
  }

  EVP_MAC_CTX_free(ctx);
  EVP_MAC_free(mac);

  if (out_len != SHA256_LEN) {
    throw std::runtime_error("Unexpected HMAC-SHA256 length");
  }

  return out;
}

}  // namespace

std::vector<uint8_t> hmac_sha256(std::span<const uint8_t> key,
                                 std::span<const uint8_t> data) {
  std::vector<std::span<const uint8_t>> parts{data};
  return hmac_sha256_impl(key, parts);
}

std::vector<uint8_t> hkdf_extract(std::span<const uint8_t> salt,
                                  std::span<const uint8_t> ikm) {
  std::array<uint8_t, SHA256_LEN> zero_salt{};
  std::span<const uint8_t> real_salt =
      salt.empty()
          ? std::span<const uint8_t>(zero_salt.data(), zero_salt.size())
          : salt;

  return hmac_sha256(real_salt, ikm);  // PRK
}

std::vector<uint8_t> hkdf_expand(std::span<const uint8_t> prk,
                                 std::span<const uint8_t> info, size_t length) {
  if (prk.size() != SHA256_LEN) {
    throw std::runtime_error("PRK must be 32 bytes for HKDF-SHA256");
  }
  if (length == 0) {
    return {};
  }
  const size_t max_len = 255 * SHA256_LEN;
  if (length > max_len) {
    throw std::runtime_error("HKDF-Expand length too large");
  }

  size_t n = (length + SHA256_LEN - 1) / SHA256_LEN;
  std::vector<uint8_t> okm;
  okm.reserve(n * SHA256_LEN);

  std::vector<uint8_t> previous;

  for (size_t i = 1; i <= n; ++i) {
    std::vector<uint8_t> hmac_input;
    hmac_input.reserve(previous.size() + info.size() + 1);

    if (!previous.empty()) {
      hmac_input.insert(hmac_input.end(), previous.begin(), previous.end());
    }
    if (!info.empty()) {
      hmac_input.insert(hmac_input.end(), info.begin(), info.end());
    }
    hmac_input.push_back(static_cast<uint8_t>(i));

    previous = hmac_sha256(prk, hmac_input);
    okm.insert(okm.end(), previous.begin(), previous.end());
  }

  okm.resize(length);
  return okm;
}

std::vector<uint8_t> hkdf_sha256(std::span<const uint8_t> salt,
                                 std::span<const uint8_t> ikm,
                                 std::span<const uint8_t> info, size_t length) {
  auto prk = hkdf_extract(salt, ikm);
  return hkdf_expand(prk, info, length);
}

}  // namespace crypto