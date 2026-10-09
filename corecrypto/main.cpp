#include <spdlog/spdlog.h>

#include <iostream>

#include "corecrypto.h"

using std::string;

int main() {
  spdlog::set_level(spdlog::level::debug);
  spdlog::info("starting...");

  CoreCrypto corecrypto;

  CoreCrypto::KeyPair alice = CoreCrypto::generate_dh_keypair();
  CoreCrypto::KeyPair bob = CoreCrypto::generate_dh_keypair();

  CoreCrypto::SessionKeys alice_shared =
      CoreCrypto::derive_dh_keys(true, alice.pubkey, alice.privkey, bob.pubkey);
  CoreCrypto::SessionKeys bob_shared =
      CoreCrypto::derive_dh_keys(false, bob.pubkey, bob.privkey, alice.pubkey);

  string alice_shared_s =
      string(alice_shared.send_key.begin(), alice_shared.send_key.end());
  string bob_shared_s =
      string(bob_shared.receive_key.begin(), bob_shared.receive_key.end());

  spdlog::debug(CoreCrypto::to_hex(alice_shared.send_key));
  spdlog::debug(CoreCrypto::to_hex(bob_shared.receive_key));

  spdlog::debug(CoreCrypto::to_hex(alice_shared.receive_key));
  spdlog::debug(CoreCrypto::to_hex(bob_shared.send_key));

  CoreCrypto::secure_memory_zero(alice_shared.receive_key);
  CoreCrypto::secure_memory_zero(alice_shared.send_key);
  CoreCrypto::secure_memory_zero(bob_shared.receive_key);
  CoreCrypto::secure_memory_zero(bob_shared.send_key);

  auto mnemonic = CoreCrypto::generate_mnemonicphrase();
  spdlog::debug("Mnemonic: {}", mnemonic);
  spdlog::debug("Seed: {}", CoreCrypto::to_hex(
                                CoreCrypto::mnemonicphrase_to_seed(mnemonic)));

  std::vector<uint8_t> seed = CoreCrypto::mnemonicphrase_to_seed(mnemonic);

  CoreCrypto::Ed25519KeyPair keypair = CoreCrypto::seed_to_keypair(seed);

  spdlog::info("Derived privkey: {}", CoreCrypto::to_hex(keypair.privkey));
  spdlog::info("Derived pubkey (compressed): {}",
               CoreCrypto::to_hex(keypair.pubkey));

  spdlog::info("Fingerprint: {}", CoreCrypto::fingerprint(keypair.pubkey));

  std::vector<uint8_t> key =
      CoreCrypto::random(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
  std::vector<uint8_t> nonce =
      CoreCrypto::random(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);

  spdlog::debug("key = {}", CoreCrypto::to_hex(key));
  spdlog::debug("nonce = {}", CoreCrypto::to_hex(nonce));

  string s = "Hello, World !!!";
  std::vector<uint8_t> plaintext(s.begin(), s.end());

  std::vector<uint8_t> ciphertext = CoreCrypto::encrypt(plaintext, key, nonce);

  std::vector<uint8_t> decrypted = CoreCrypto::decrypt(ciphertext, key, nonce);
  string result(decrypted.begin(), decrypted.end());

  spdlog::info("Result: {}", result);

  CoreCrypto::secure_memory_zero(key);
  CoreCrypto::secure_memory_zero(nonce);

  spdlog::info("all tests passed successfully");

  CoreCrypto a;

  return 0;
}