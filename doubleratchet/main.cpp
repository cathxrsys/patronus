#include <spdlog/spdlog.h>

#include <array>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "corecrypto.h"
#include "coreutils.h"
#include "crypto.h"
#include "doubleratchet.h"
#include "pq.h"
#include "pqdh.h"
#include "prekeys.h"

static CoreCrypto::SignKeyPair load_cert() {
  nlohmann::json certificate;

  try {
    std::ifstream certificateFile("certificate.json");
    if (!certificateFile) {
      throw std::runtime_error("cannot open certificate.json");
    }
    certificate = nlohmann::json::parse(certificateFile, nullptr, true, true);
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("failed to read certificate.json: ") +
                             e.what());
  }

  if (!certificate.contains("privkey") || !certificate.contains("pubkey"))
    throw std::runtime_error("certificate.json missing privkey or pubkey");

  const std::string base64_priv = certificate.at("privkey").get<std::string>();
  const std::string base64_pub = certificate.at("pubkey").get<std::string>();

  std::vector<uint8_t> privkey;
  std::vector<uint8_t> pubkey;

  CoreCrypto::from_base64(base64_priv, privkey);
  CoreCrypto::from_base64(base64_pub, pubkey);

  CoreCrypto::SignKeyPair result{privkey, pubkey};
  return result;
}

int main() {
  spdlog::set_level(spdlog::level::debug);
  CoreCrypto a;

  CoreCrypto::SignKeyPair sign_keypair = load_cert();

  spdlog::info("Loaded certificate with pubkey: {}",
               coreutils::bytes_to_hex(sign_keypair.pubkey));

  using namespace dr;

  Key32 initial_root_key{};
  for (size_t i = 0; i < KEY_LEN; ++i)
        initial_root_key[i] = i;  // just for testing

    // Generate initial PQDH keys for both sides

  pqdh::KeyPairs alice_pqdh = pqdh::generate_keypairs();
  pqdh::KeyPairs bob_pqdh = pqdh::generate_keypairs();

  spdlog::info("Initial root key alice: {}",
               coreutils::bytes_to_hex(initial_root_key.data(),
                                       initial_root_key.size()));
  spdlog::info("Initial root key bob:   {}",
               coreutils::bytes_to_hex(initial_root_key.data(),
                                       initial_root_key.size()));

  DoubleRatchet alice, bob;

  spdlog::info("Initializing DoubleRatchet instances...");

  std::vector<uint8_t> encapsulated_ciphertext = alice.pq_init(
      initial_root_key, alice_pqdh, bob_pqdh.dh_pub, bob_pqdh.pq_pk, true);

  spdlog::info("Alice PQDH init done.");
  spdlog::info("Bob PQDH init starting...");

  bob.pq_init(initial_root_key, bob_pqdh, alice_pqdh.dh_pub,
              encapsulated_ciphertext, false);

  spdlog::info("DoubleRatchet initialized.");

  spdlog::info(
      "Alice UID: {}",
      coreutils::bytes_to_hex(alice.get_uid().data(), alice.get_uid().size()));
  spdlog::info("Bob UID:   {}", coreutils::bytes_to_hex(bob.get_uid().data(),
                                                        bob.get_uid().size()));

  assert(alice.get_uid() == bob.get_uid() &&
         "UIDs must be == after initialization");

  spdlog::info("Alice root key: {}",
               coreutils::bytes_to_hex(alice._debug_root_key().data(),
                                       alice._debug_root_key().size()));
  spdlog::info("Bob root key:   {}",
               coreutils::bytes_to_hex(bob._debug_root_key().data(),
                                       bob._debug_root_key().size()));

  assert(alice._debug_root_key() == bob._debug_root_key() &&
         "Root keys must be == after initialization");

  spdlog::info("Alice chain key send:    {}",
               coreutils::bytes_to_hex(alice._debug_chain_key_send().data(),
                                       alice._debug_chain_key_send().size()));
  spdlog::info(
      "Alice chain key receive: {}",
      coreutils::bytes_to_hex(alice._debug_chain_key_receive().data(),
                              alice._debug_chain_key_receive().size()));

  spdlog::info("Bob chain key send:      {}",
               coreutils::bytes_to_hex(bob._debug_chain_key_send().data(),
                                       bob._debug_chain_key_send().size()));
  spdlog::info("Bob chain key receive:   {}",
               coreutils::bytes_to_hex(bob._debug_chain_key_receive().data(),
                                       bob._debug_chain_key_receive().size()));

  assert(alice._debug_chain_key_send() == bob._debug_chain_key_receive() &&
         "Alice send chain key must == Bob receive chain key");
  assert(alice._debug_chain_key_receive() == bob._debug_chain_key_send() &&
         "Alice receive chain key must == Bob send chain key");

  Key32 alice_msg_key1_1, bob_msg_key1_1;
  alice.ratchet_send(alice_msg_key1_1);
  bob.ratchet_receive(bob_msg_key1_1);

  spdlog::info("Alice message key 1_1: {}",
               coreutils::bytes_to_hex(alice_msg_key1_1.data(),
                                       alice_msg_key1_1.size()));
  spdlog::info(
      "Bob message key 1_1:   {}",
      coreutils::bytes_to_hex(bob_msg_key1_1.data(), bob_msg_key1_1.size()));

  assert(alice_msg_key1_1 == bob_msg_key1_1 &&
         "Alice and Bob message keys must be == after ratchet send/receive");

  std::string test_str = "Hello, DoubleRatchet!";
  std::vector<uint8_t> test_data =
      std::vector<uint8_t>(test_str.begin(), test_str.end());
  EncryptedMessage encrypted_data_alice = alice.encrypt(test_data);
  std::vector<uint8_t> decrypted_data_bob = bob.decrypt(encrypted_data_alice);

  EncryptedMessage encrypted_data_alice2 = alice.encrypt(test_data);
  std::vector<uint8_t> decrypted_data_bob2 = bob.decrypt(encrypted_data_alice2);

  spdlog::info(
      "Decrypted data by Bob: {}",
      std::string(decrypted_data_bob.begin(), decrypted_data_bob.end()));
  spdlog::info(
      "Decrypted data by Bob2: {}",
      std::string(decrypted_data_bob2.begin(), decrypted_data_bob2.end()));
  assert(decrypted_data_bob == test_data &&
         "Decrypted data by Bob must match original test data");

  std::string session_key_fingerprint_alice =
      alice.get_session_key_fingerprint();
  std::string session_key_fingerprint_bob = bob.get_session_key_fingerprint();

  spdlog::info("Alice session key fingerprint: {}",
               session_key_fingerprint_alice);
  spdlog::info("Bob   session key fingerprint: {}",
               session_key_fingerprint_bob);

  assert(session_key_fingerprint_alice == session_key_fingerprint_bob &&
         "Session key fingerprints must match after first ratchet");

  Key32 alice_msg_key1_2, bob_msg_key1_2;
  alice.ratchet_send(alice_msg_key1_2);
  bob.ratchet_receive(bob_msg_key1_2);

  spdlog::info("Alice message key 1_2: {}",
               coreutils::bytes_to_hex(alice_msg_key1_2.data(),
                                       alice_msg_key1_2.size()));
  spdlog::info(
      "Bob message key 1_2:   {}",
      coreutils::bytes_to_hex(bob_msg_key1_2.data(), bob_msg_key1_2.size()));

  assert(alice_msg_key1_2 == bob_msg_key1_2 &&
         "Alice and Bob message keys must be == after ratchet send/receive");

  dr::MessageUID alice_send_uid = alice.get_send_message_uid();
  dr::MessageUID bob_recv_uid = bob.get_recv_message_uid();

  std::ostringstream oss;
  oss << "Alice send UID: " << alice_send_uid;
  spdlog::info(oss.str());

  std::ostringstream oss2;
  oss2 << "Bob recv UID:   " << bob_recv_uid;
  spdlog::info(oss2.str());

  assert(alice_send_uid == bob_recv_uid &&
         "Alice send UID must == Bob recv UID");

  CoreCrypto::DHKeyPair alice_dh = CoreCrypto::generate_dh_keypair();
  CoreCrypto::DHKeyPair bob_dh = CoreCrypto::generate_dh_keypair();

  DHKeyPair alice_dh_pair;
  std::copy_n(alice_dh.privkey.data(), KEY_LEN, alice_dh_pair.priv.begin());
  std::copy_n(alice_dh.pubkey.data(), KEY_LEN, alice_dh_pair.pub.begin());

  DHKeyPair bob_dh_pair;
  std::copy_n(bob_dh.privkey.data(), KEY_LEN, bob_dh_pair.priv.begin());
  std::copy_n(bob_dh.pubkey.data(), KEY_LEN, bob_dh_pair.pub.begin());

  alice.dh_ratchet(alice_dh_pair, bob_dh_pair.pub);
  bob.dh_ratchet(bob_dh_pair, alice_dh_pair.pub);

  spdlog::info("DH ratchet rotated.");

  Key32 alice_msg_key2_1, bob_msg_key2_1;
  alice.ratchet_send(alice_msg_key2_1);
  bob.ratchet_receive(bob_msg_key2_1);

  spdlog::info("Alice message key 2_1: {}",
               coreutils::bytes_to_hex(alice_msg_key2_1.data(),
                                       alice_msg_key2_1.size()));
  spdlog::info(
      "Bob message key 2_1:   {}",
      coreutils::bytes_to_hex(bob_msg_key2_1.data(), bob_msg_key2_1.size()));

  assert(alice_msg_key2_1 == bob_msg_key2_1 &&
         "Alice and Bob message keys must be == after DH ratchet send/receive");

  Key32 alice_msg_key2_2, bob_msg_key2_2;
  alice.ratchet_send(alice_msg_key2_2);
  bob.ratchet_receive(bob_msg_key2_2);

  spdlog::info("Alice message key 2_2: {}",
               coreutils::bytes_to_hex(alice_msg_key2_2.data(),
                                       alice_msg_key2_2.size()));
  spdlog::info(
      "Bob message key 2_2:   {}",
      coreutils::bytes_to_hex(bob_msg_key2_2.data(), bob_msg_key2_2.size()));

  assert(alice_msg_key2_2 == bob_msg_key2_2 &&
         "Alice and Bob message keys must be == after DH ratchet send/receive");

  spdlog::info("Alice DH ratchet step: {}", alice.get_dh_ratchet_step());
  spdlog::info("Bob DH ratchet step:   {}", bob.get_dh_ratchet_step());

  assert(alice.get_dh_ratchet_step() == bob.get_dh_ratchet_step() &&
         "DH ratchet steps must be == after DH ratchet");

  spdlog::info("Alice seq num send: {}", alice.get_seq_num_send());
  spdlog::info("Bob seq num recv:   {}", bob.get_seq_num_recv());

  assert(alice.get_seq_num_send() == bob.get_seq_num_recv() &&
         "Alice seq num send must == Bob seq num recv");

  spdlog::info("Alice seq num recv: {}", alice.get_seq_num_recv());
  spdlog::info("Bob seq num send:   {}", bob.get_seq_num_send());

  assert(alice.get_seq_num_recv() == bob.get_seq_num_send() &&
         "Alice seq num recv must == Bob seq num send");

  spdlog::info(
      "================= Serialization Test for Alice ================");
  spdlog::info("Old Alice data...");
  alice.debug_print();

  nlohmann::json alice_json = alice.to_json();
  spdlog::info("Serialized Alice data: {}", alice_json.dump());

  DoubleRatchet alice_copy;
  if (alice_copy.from_json(alice_json)) {
    spdlog::info("Deserialized Alice data...");
    alice_copy.debug_print();
  } else {
    spdlog::error("Failed to deserialize Alice data.");
  }

  spdlog::info("All tests for DoubleRatchet algorithm passed successfully!");

  spdlog::info("================= PQ KEM Test =================");

  pq::KeyPair alice_pq_keypair = pq::kem_generate_keypair();

  spdlog::info("PQ KEM Key Pair for Alice generated successfully!");
  spdlog::info("Public Key: {}",
               coreutils::bytes_to_hex(alice_pq_keypair.pk.data(),
                                       alice_pq_keypair.pk.size()));
  spdlog::info("Secret Key: {}",
               coreutils::bytes_to_hex(alice_pq_keypair.sk.data(),
                                       alice_pq_keypair.sk.size()));

  pq::KeyPair bob_pq_keypair = pq::kem_generate_keypair();

  spdlog::info("PQ KEM Key Pair for Bob generated successfully!");
  spdlog::info("Public Key: {}",
               coreutils::bytes_to_hex(bob_pq_keypair.pk.data(),
                                       bob_pq_keypair.pk.size()));
  spdlog::info("Secret Key: {}",
               coreutils::bytes_to_hex(bob_pq_keypair.sk.data(),
                                       bob_pq_keypair.sk.size()));

  spdlog::info(
      "Encapsulating shared secret for Alice using Bob's public key...");

  pq::Encapsulated encapsulated_for_alice =
      pq::kem_encapsulate(bob_pq_keypair.pk);
  spdlog::info("Ciphertext: {}", coreutils::bytes_to_hex(
                                     encapsulated_for_alice.ciphertext.data(),
                                     encapsulated_for_alice.ciphertext.size()));
  spdlog::info(
      "Shared Secret: {}",
      coreutils::bytes_to_hex(encapsulated_for_alice.shared_secret.data(),
                              encapsulated_for_alice.shared_secret.size()));

  spdlog::info("Decapsulating shared secret for Bob using his secret key...");

  std::vector<uint8_t> decapsulated_secret_for_bob =
      pq::kem_decapsulate(encapsulated_for_alice.ciphertext, bob_pq_keypair.sk);
  spdlog::info("Decapsulated Shared Secret: {}",
               coreutils::bytes_to_hex(decapsulated_secret_for_bob.data(),
                                       decapsulated_secret_for_bob.size()));

  assert(encapsulated_for_alice.shared_secret == decapsulated_secret_for_bob &&
         "Shared secrets must match after KEM encapsulation/decapsulation");

  spdlog::info("PQ KEM test passed successfully!");

  spdlog::info("========== PQDH Hybrid Key Exchange Test ==========");

  pqdh::KeyPairs alice_keys = pqdh::generate_keypairs();
  pqdh::KeyPairs bob_keys = pqdh::generate_keypairs();

  pq::Encapsulated encapsulated_for_bob =
      pqdh::make_encapsulated(bob_keys.pq_pk);

  auto alice_dh_secret =
      CoreCrypto::derive_dh_secret(alice_keys.dh_priv, bob_keys.dh_pub);

  // 4. Alice combines PQ and DH secrets
  std::vector<uint8_t> alice_shared_key = pqdh::make_shared_key(
      encapsulated_for_bob.shared_secret,
      std::vector<uint8_t>(alice_dh_secret.begin(), alice_dh_secret.end()));

  pqdh::SharedSecrets bob_secrets = pqdh::derive_shared_secrets(
      encapsulated_for_bob.ciphertext, bob_keys.pq_sk, bob_keys.dh_priv,
      alice_keys.dh_pub);

  // 6. Bob combines PQ and DH secrets
  std::vector<uint8_t> bob_shared_key = pqdh::make_shared_key(
      bob_secrets.pq_shared_secret, bob_secrets.dh_shared_secret);

  spdlog::info("Alice PQDH shared key: {}",
               coreutils::bytes_to_hex(alice_shared_key.data(),
                                       alice_shared_key.size()));
  spdlog::info(
      "Bob   PQDH shared key: {}",
      coreutils::bytes_to_hex(bob_shared_key.data(), bob_shared_key.size()));

  assert(alice_shared_key == bob_shared_key && "PQDH shared keys must match!");

  spdlog::info("PQDH hybrid key exchange test passed successfully!");

  // PreKeys test

  spdlog::info("prekeys.cpp test...");

  std::vector<uint8_t> dh_pub = bob_keys.dh_pub;
  std::vector<uint8_t> pq_pub = bob_keys.pq_pk;
  std::vector<uint8_t> Ed25519_pub = sign_keypair.pubkey;

  spdlog::info("dh_pub: {}", coreutils::bytes_to_hex(dh_pub));
  spdlog::info("pq_pub: {}", coreutils::bytes_to_hex(pq_pub));

  spdlog::info("pq_pub len = {}", pq_pub.size());

  spdlog::info("Ed25519_pub: {}", coreutils::bytes_to_hex(Ed25519_pub));

  std::vector<uint8_t> _id;
  _id.reserve(dh_pub.size() + pq_pub.size() + Ed25519_pub.size());
  _id.insert(_id.end(), dh_pub.begin(), dh_pub.end());
  _id.insert(_id.end(), pq_pub.begin(), pq_pub.end());
  _id.insert(_id.end(), Ed25519_pub.begin(), Ed25519_pub.end());

  std::vector<uint8_t> id = CoreCrypto::sha256(_id);

  std::vector<uint8_t> signed_data;
  signed_data.reserve(id.size() + dh_pub.size() + pq_pub.size() +
                      Ed25519_pub.size());
  signed_data.insert(signed_data.end(), id.begin(), id.end());
  signed_data.insert(signed_data.end(), dh_pub.begin(), dh_pub.end());
  signed_data.insert(signed_data.end(), pq_pub.begin(), pq_pub.end());
  signed_data.insert(signed_data.end(), Ed25519_pub.begin(), Ed25519_pub.end());

  prekeys::PreKey prekey =
      prekeys::to_prekey(id, dh_pub, pq_pub, Ed25519_pub,
                         CoreCrypto::sign(sign_keypair.privkey, signed_data));

  std::string prekey_json = prekeys::prekey_to_json(prekey);

  spdlog::info("PreKey JSON: {}", prekey_json);

  prekeys::PreKey parsed_prekey = prekeys::json_to_prekey(prekey_json);

  bool verify_result = prekeys::verify_prekey(parsed_prekey);

  spdlog::info("PreKey verification result: {}",
               verify_result ? "valid" : "invalid");

  spdlog::info("All tests completed successfully.");

  // ================= DH Ratchet on Direction Change Tests =================
    // Tests use ONLY encrypt/decrypt, without manually calling ratchet.
    // The DH ratchet advances automatically when the direction changes.
  {
    spdlog::info(
        "================= DH Ratchet on Direction Change Test "
        "=================");

    // --- Initialize a fresh Alice2/Bob2 pair ---
    pqdh::KeyPairs alice2_pqdh = pqdh::generate_keypairs();
    pqdh::KeyPairs bob2_pqdh = pqdh::generate_keypairs();

    DoubleRatchet alice2, bob2;

    std::vector<uint8_t> ct = alice2.pq_init(
        initial_root_key, alice2_pqdh, bob2_pqdh.dh_pub, bob2_pqdh.pq_pk, true);
    bob2.pq_init(initial_root_key, bob2_pqdh, alice2_pqdh.dh_pub, ct, false);

    assert(alice2.get_uid() == bob2.get_uid() && "UIDs must match");

    auto to_vec = [](const std::string& s) {
      return std::vector<uint8_t>(s.begin(), s.end());
    };
    auto to_str = [](const std::vector<uint8_t>& v) {
      return std::string(v.begin(), v.end());
    };

    // ---- 1. Alice sends several messages in a row (same direction) ----
    spdlog::info("--- Phase 1: Alice -> Bob (3 messages, same direction) ---");

    uint32_t step_before = alice2.get_dh_ratchet_step();

    EncryptedMessage a2b_1 = alice2.encrypt(to_vec("Hello Bob!"));
    EncryptedMessage a2b_2 = alice2.encrypt(to_vec("How are you?"));
    EncryptedMessage a2b_3 = alice2.encrypt(to_vec("Are you there?"));

    // First message after init: Alice is the initiator, last_action_was_send =
    // true, so the first encrypt must NOT advance the DH ratchet (direction
    // did not change). All three messages share the same dh_ratchet_step.
    assert(a2b_1.dh_ratchet_step == a2b_2.dh_ratchet_step &&
           "Same direction — same DH step");
    assert(a2b_2.dh_ratchet_step == a2b_3.dh_ratchet_step &&
           "Same direction — same DH step");

    std::vector<uint8_t> plain1 = bob2.decrypt(a2b_1);
    std::vector<uint8_t> plain2 = bob2.decrypt(a2b_2);
    std::vector<uint8_t> plain3 = bob2.decrypt(a2b_3);

    assert(to_str(plain1) == "Hello Bob!" && "Decryption 1 failed");
    assert(to_str(plain2) == "How are you?" && "Decryption 2 failed");
    assert(to_str(plain3) == "Are you there?" && "Decryption 3 failed");

    spdlog::info("Phase 1 OK: 3 messages Alice->Bob decrypted correctly.");

    // ---- 2. Bob replies (direction change -> automatic DH ratchet) ----
    spdlog::info(
        "--- Phase 2: Bob -> Alice (direction change, DH ratchet expected) "
        "---");

    uint32_t bob_step_before_reply = bob2.get_dh_ratchet_step();

    EncryptedMessage b2a_1 = bob2.encrypt(to_vec("Hi Alice!"));
    EncryptedMessage b2a_2 = bob2.encrypt(to_vec("I'm fine, thanks"));

    // Bob switched from receiving to sending -> the DH ratchet should have
    // advanced
    assert(bob2.get_dh_ratchet_step() == bob_step_before_reply + 1 &&
           "Bob DH ratchet step must increment on direction change");

    // Both Bob messages share the same (new) dh_ratchet_step
    assert(b2a_1.dh_ratchet_step == b2a_2.dh_ratchet_step &&
           "Same direction — same DH step");

    // Alice decrypts Bob's replies (her decrypt sees a new dh_pub ->
    // auto-ratchet)
    std::vector<uint8_t> plain4 = alice2.decrypt(b2a_1);
    std::vector<uint8_t> plain5 = alice2.decrypt(b2a_2);

    assert(to_str(plain4) == "Hi Alice!" && "Decryption 4 failed");
    assert(to_str(plain5) == "I'm fine, thanks" && "Decryption 5 failed");

    // After decrypt, Alice should also be on the same dh_ratchet_step as Bob
    assert(alice2.get_dh_ratchet_step() == bob2.get_dh_ratchet_step() &&
           "DH ratchet steps must match after direction change");

    spdlog::info(
        "Phase 2 OK: direction change Bob->Alice, DH ratchet verified.");

    // ---- 3. Alice replies again (another direction change) ----
    spdlog::info("--- Phase 3: Alice -> Bob (another direction change) ---");

    uint32_t alice_step_before = alice2.get_dh_ratchet_step();

    EncryptedMessage a2b_4 = alice2.encrypt(to_vec("Great to hear!"));
    EncryptedMessage a2b_5 = alice2.encrypt(to_vec("Let's meet tomorrow"));

    // Alice's OWN key was already regenerated when she decrypted Bob's new
    // key back in Phase 2 (the atomic DH ratchet does both halves at once,
    // right there in decrypt() — not lazily deferred to the next send). So
    // replying here reacts to nothing new on Alice's side: her step must NOT
    // move again.
    assert(alice2.get_dh_ratchet_step() == alice_step_before &&
           "Alice's step must not move again: her key was already fresh from "
           "Phase 2's atomic ratchet");
    assert(a2b_4.dh_ratchet_step == a2b_5.dh_ratchet_step &&
           "Same direction — same DH step");

    uint32_t bob_step_before_a4 = bob2.get_dh_ratchet_step();

    std::vector<uint8_t> plain6 = bob2.decrypt(a2b_4);
    std::vector<uint8_t> plain7 = bob2.decrypt(a2b_5);

    assert(to_str(plain6) == "Great to hear!" && "Decryption 6 failed");
    assert(to_str(plain7) == "Let's meet tomorrow" && "Decryption 7 failed");

    // Bob, in contrast, is seeing Alice's Phase-2-generated key for the first
    // time in a2b_4 — that IS new to him, so his own atomic ratchet fires
    // exactly once (not again for a2b_5, which carries the same key).
    assert(bob2.get_dh_ratchet_step() == bob_step_before_a4 + 1 &&
           "Bob's step must advance exactly once: Alice's key is new to him");

    spdlog::info(
        "Phase 3 OK: direction change Alice->Bob, DH ratchet verified "
        "(alice={}, bob={} — expected to differ until the next reply closes "
        "the loop).",
        alice2.get_dh_ratchet_step(), bob2.get_dh_ratchet_step());

    // ---- 4. Rapid alternation: A->B, B->A, A->B, B->A (one message each) ----
    spdlog::info("--- Phase 4: Rapid direction changes (ping-pong) ---");

    // In strict single-message alternation, every message carries a key the
    // recipient has never seen (each side's previous atomic ratchet just
    // freshly generated it) — so each decrypt triggers exactly one atomic
    // ratchet on the RECEIVING side. The two per-side counters advance in
    // lockstep-with-a-one-message-lag, not in numeric equality at every point.
    auto step = [&] { return std::pair(alice2.get_dh_ratchet_step(),
                                       bob2.get_dh_ratchet_step()); };
    auto [a0, b0] = step();

    // Bob → Alice
    EncryptedMessage ping1 = bob2.encrypt(to_vec("ping1"));
    assert(bob2.get_dh_ratchet_step() == b0 &&
           "Bob's own key was already fresh, sending doesn't ratchet");
    std::vector<uint8_t> pong1_pt = alice2.decrypt(ping1);
    assert(to_str(pong1_pt) == "ping1" && "ping1 failed");
    assert(alice2.get_dh_ratchet_step() == a0 + 1 &&
           "Alice ratchets once: Bob's key was new to her");

    // Alice → Bob
    EncryptedMessage ping2 = alice2.encrypt(to_vec("pong1"));
    std::vector<uint8_t> pong2_pt = bob2.decrypt(ping2);
    assert(to_str(pong2_pt) == "pong1" && "pong1 failed");
    assert(bob2.get_dh_ratchet_step() == b0 + 1 &&
           "Bob ratchets once: Alice's key was new to him");

    // Bob → Alice
    EncryptedMessage ping3 = bob2.encrypt(to_vec("ping2"));
    std::vector<uint8_t> pong3_pt = alice2.decrypt(ping3);
    assert(to_str(pong3_pt) == "ping2" && "ping2 failed");
    assert(alice2.get_dh_ratchet_step() == a0 + 2 &&
           "Alice ratchets again: Bob's newest key is new to her");

    // Alice → Bob
    EncryptedMessage ping4 = alice2.encrypt(to_vec("pong2"));
    std::vector<uint8_t> pong4_pt = bob2.decrypt(ping4);
    assert(to_str(pong4_pt) == "pong2" && "pong2 failed");
    assert(bob2.get_dh_ratchet_step() == b0 + 2 &&
           "Bob ratchets again: Alice's newest key is new to him");

    // Both sides end this exact interleaving one message apart (Alice always
    // reacts to Bob before Bob gets to react to Alice's reply) — that gap is
    // expected, not a desync; message content above already proves both
    // directions decrypt correctly throughout.
    spdlog::info(
        "Phase 4 OK: 4 rapid direction changes, each triggered exactly one "
        "atomic ratchet on the receiving side (alice={}, bob={}).",
        alice2.get_dh_ratchet_step(), bob2.get_dh_ratchet_step());

    // ---- 5. Serialize/Deserialize and validate messages via wire format ----
    spdlog::info(
        "--- Phase 5: Serialize/Deserialize EncryptedMessage with dh_pub ---");

    EncryptedMessage wire_msg = bob2.encrypt(to_vec("wire test"));
    std::vector<uint8_t> wire = wire_msg.serialize();
    EncryptedMessage wire_msg2 = EncryptedMessage::deserialize(wire);

    assert(wire_msg.uid == wire_msg2.uid && "Serialize: UID mismatch");
    assert(wire_msg.dh_ratchet_step == wire_msg2.dh_ratchet_step &&
           "Serialize: dh_ratchet_step mismatch");
    assert(wire_msg.seq_num == wire_msg2.seq_num &&
           "Serialize: seq_num mismatch");
    assert(wire_msg.prev_chain_len == wire_msg2.prev_chain_len &&
           "Serialize: prev_chain_len mismatch");
    assert(wire_msg.dh_pub == wire_msg2.dh_pub && "Serialize: dh_pub mismatch");
    assert(wire_msg.nonce == wire_msg2.nonce && "Serialize: nonce mismatch");
    assert(wire_msg.ciphertext == wire_msg2.ciphertext &&
           "Serialize: ciphertext mismatch");

    std::vector<uint8_t> wire_plain = alice2.decrypt(wire_msg2);
    assert(to_str(wire_plain) == "wire test" &&
           "Decrypt after deserialize failed");

    spdlog::info(
        "Phase 5 OK: EncryptedMessage serialize/deserialize with dh_pub "
        "works.");

    // ---- 6. JSON session serialization after several ratchet steps ----
    spdlog::info(
        "--- Phase 6: Session JSON save/restore and continue chat ---");

    nlohmann::json alice2_json = alice2.to_json();
    nlohmann::json bob2_json = bob2.to_json();

    DoubleRatchet alice3, bob3;
    assert(alice3.from_json(alice2_json) && "Failed to restore Alice session");
    assert(bob3.from_json(bob2_json) && "Failed to restore Bob session");

    // Continue the conversation after restoring the session
    EncryptedMessage after_restore_1 =
        alice3.encrypt(to_vec("Restored Alice here"));
    std::vector<uint8_t> ar_plain1 = bob3.decrypt(after_restore_1);
    assert(to_str(ar_plain1) == "Restored Alice here" &&
           "Post-restore decrypt 1 failed");

    EncryptedMessage after_restore_2 =
        bob3.encrypt(to_vec("Restored Bob here"));
    std::vector<uint8_t> ar_plain2 = alice3.decrypt(after_restore_2);
    assert(to_str(ar_plain2) == "Restored Bob here" &&
           "Post-restore decrypt 2 failed");

    assert(alice3.get_dh_ratchet_step() == bob3.get_dh_ratchet_step() &&
           "DH steps must match after restore");

    spdlog::info("Phase 6 OK: Session restore and continued chat works.");

    // ---- 7. CRITICAL: Offline queue scenario (root cause of the bug) ----
    // Alice sends 3 messages while Bob is offline.
    // Bob receives the 1st, sends "received", then receives the 2nd and 3rd.
    // In the old implementation (full dh_ratchet), Bob's reply broke the receive chain.
    spdlog::info("--- Phase 7: Offline queue scenario (the bug scenario) ---");
    {
      pqdh::KeyPairs a7_pqdh = pqdh::generate_keypairs();
      pqdh::KeyPairs b7_pqdh = pqdh::generate_keypairs();

      DoubleRatchet a7, b7;
      std::vector<uint8_t> ct7 = a7.pq_init(
          initial_root_key, a7_pqdh, b7_pqdh.dh_pub, b7_pqdh.pq_pk, true);
      b7.pq_init(initial_root_key, b7_pqdh, a7_pqdh.dh_pub, ct7, false);

    // Alice sends 3 messages (Bob is offline, messages stay on the server)
      EncryptedMessage q1 = a7.encrypt(to_vec("msg1"));
      EncryptedMessage q2 = a7.encrypt(to_vec("msg2"));
      EncryptedMessage q3 = a7.encrypt(to_vec("msg3"));

      spdlog::info("Alice sent 3 messages to offline Bob.");

    // Bob connects and receives the 1st message
      std::vector<uint8_t> p1 = b7.decrypt(q1);
      assert(to_str(p1) == "msg1" && "Offline queue: msg1 decrypt failed");
      spdlog::info("Bob decrypted msg1 OK.");

    // Bob sends an acknowledgment (direction change!)
      EncryptedMessage ack = b7.encrypt(to_vec("received"));
      spdlog::info(
          "Bob sent 'received' ack (direction change, send half-ratchet).");

    // Bob receives the 2nd and 3rd messages (their dh_pub did not change)
      std::vector<uint8_t> p2 = b7.decrypt(q2);
      assert(to_str(p2) == "msg2" &&
             "Offline queue: msg2 decrypt failed after ack");
      spdlog::info("Bob decrypted msg2 OK (after sending ack).");

      std::vector<uint8_t> p3 = b7.decrypt(q3);
      assert(to_str(p3) == "msg3" &&
             "Offline queue: msg3 decrypt failed after ack");
      spdlog::info("Bob decrypted msg3 OK (after sending ack).");

    // Alice receives Bob's acknowledgment
      std::vector<uint8_t> ack_plain = a7.decrypt(ack);
      assert(to_str(ack_plain) == "received" &&
             "Offline queue: ack decrypt failed");
      spdlog::info("Alice decrypted Bob's ack OK.");

    // The conversation continues normally
      EncryptedMessage cont1 = a7.encrypt(to_vec("got your ack"));
      std::vector<uint8_t> cont1_pt = b7.decrypt(cont1);
      assert(to_str(cont1_pt) == "got your ack" &&
             "Offline queue: continuation failed");

      EncryptedMessage cont2 = b7.encrypt(to_vec("great"));
      std::vector<uint8_t> cont2_pt = a7.decrypt(cont2);
      assert(to_str(cont2_pt) == "great" &&
             "Offline queue: continuation 2 failed");

      spdlog::info("Phase 7 OK: Offline queue scenario works correctly!");
    }

    // ---- 8. Multiple offline messages in both directions ----
    // Alice sends 3, Bob replies with 2, Alice sends 2 more - all offline.
    // Then everything is delivered in order.
    spdlog::info(
        "--- Phase 8: Interleaved offline messages both directions ---");
    {
      pqdh::KeyPairs a8_pqdh = pqdh::generate_keypairs();
      pqdh::KeyPairs b8_pqdh = pqdh::generate_keypairs();

      DoubleRatchet a8, b8;
      std::vector<uint8_t> ct8 = a8.pq_init(
          initial_root_key, a8_pqdh, b8_pqdh.dh_pub, b8_pqdh.pq_pk, true);
      b8.pq_init(initial_root_key, b8_pqdh, a8_pqdh.dh_pub, ct8, false);

    // Alice sends 3 messages
      EncryptedMessage m1 = a8.encrypt(to_vec("a1"));
      EncryptedMessage m2 = a8.encrypt(to_vec("a2"));
      EncryptedMessage m3 = a8.encrypt(to_vec("a3"));

    // Bob receives all 3
      assert(to_str(b8.decrypt(m1)) == "a1");
      assert(to_str(b8.decrypt(m2)) == "a2");
      assert(to_str(b8.decrypt(m3)) == "a3");

    // Bob replies with 2 messages
      EncryptedMessage r1 = b8.encrypt(to_vec("b1"));
      EncryptedMessage r2 = b8.encrypt(to_vec("b2"));

    // Alice sends 2 more BEFORE receiving Bob's replies (offline queue)
      EncryptedMessage m4 = a8.encrypt(to_vec("a4"));
      EncryptedMessage m5 = a8.encrypt(to_vec("a5"));

    // Now Alice receives Bob's replies
      assert(to_str(a8.decrypt(r1)) == "b1");
      assert(to_str(a8.decrypt(r2)) == "b2");

    // Bob receives Alice's remaining messages
      assert(to_str(b8.decrypt(m4)) == "a4");
      assert(to_str(b8.decrypt(m5)) == "a5");

    // Verify that the conversation continues
      EncryptedMessage final1 = b8.encrypt(to_vec("all good"));
      assert(to_str(a8.decrypt(final1)) == "all good");

      spdlog::info(
          "Phase 8 OK: Interleaved offline messages both directions works!");
    }

    // ---- 9. Out-of-order delivery inside one chain (skipped keys) ----
    spdlog::info("--- Phase 9: In-chain reordering via skipped keys ---");
    {
      pqdh::KeyPairs a9_pqdh = pqdh::generate_keypairs();
      pqdh::KeyPairs b9_pqdh = pqdh::generate_keypairs();

      DoubleRatchet a9, b9;
      std::vector<uint8_t> ct9 = a9.pq_init(
          initial_root_key, a9_pqdh, b9_pqdh.dh_pub, b9_pqdh.pq_pk, true);
      b9.pq_init(initial_root_key, b9_pqdh, a9_pqdh.dh_pub, ct9, false);

      EncryptedMessage o0 = a9.encrypt(to_vec("o0"));
      EncryptedMessage o1 = a9.encrypt(to_vec("o1"));
      EncryptedMessage o2 = a9.encrypt(to_vec("o2"));
      EncryptedMessage o3 = a9.encrypt(to_vec("o3"));

      // The last message arrives first: keys for 0..2 must get banked.
      assert(to_str(b9.decrypt(o3)) == "o3" && "Reorder: o3 (first) failed");
      assert(b9.skipped_key_count() == 3 && "Reorder: expected 3 banked keys");

      // The rest arrive in arbitrary order and decrypt via the bank.
      assert(to_str(b9.decrypt(o1)) == "o1" && "Reorder: late o1 failed");
      assert(to_str(b9.decrypt(o0)) == "o0" && "Reorder: late o0 failed");
      assert(to_str(b9.decrypt(o2)) == "o2" && "Reorder: late o2 failed");
      assert(b9.skipped_key_count() == 0 && "Reorder: bank must be empty");

      // The conversation continues normally afterwards.
      EncryptedMessage r9 = b9.encrypt(to_vec("reply"));
      assert(to_str(a9.decrypt(r9)) == "reply" && "Reorder: reply failed");
      EncryptedMessage n9 = a9.encrypt(to_vec("next"));
      assert(to_str(b9.decrypt(n9)) == "next" && "Reorder: next failed");

      spdlog::info("Phase 9 OK: in-chain reordering handled.");
    }

    // ---- 10. Late message from a previous chain (PN banking) ----
    spdlog::info("--- Phase 10: Cross-chain late delivery via PN ---");
    {
      pqdh::KeyPairs a10_pqdh = pqdh::generate_keypairs();
      pqdh::KeyPairs b10_pqdh = pqdh::generate_keypairs();

      DoubleRatchet a10, b10;
      std::vector<uint8_t> ct10 = a10.pq_init(
          initial_root_key, a10_pqdh, b10_pqdh.dh_pub, b10_pqdh.pq_pk, true);
      b10.pq_init(initial_root_key, b10_pqdh, a10_pqdh.dh_pub, ct10, false);

      // Alice's first chain: two messages, the second one is delayed in
      // transit.
      EncryptedMessage x0 = a10.encrypt(to_vec("x0"));
      EncryptedMessage x1 = a10.encrypt(to_vec("x1"));  // delayed

      assert(to_str(b10.decrypt(x0)) == "x0" && "PN: x0 failed");

      // Bob replies; Alice ratchets and starts a new chain (PN = 2).
      EncryptedMessage y0 = b10.encrypt(to_vec("y0"));
      assert(to_str(a10.decrypt(y0)) == "y0" && "PN: y0 failed");

      EncryptedMessage z0 = a10.encrypt(to_vec("z0"));
      assert(z0.prev_chain_len == 2 && "PN: z0 must carry PN=2");

      // Bob sees the new chain; PN makes him bank the key x1 still owes.
      assert(to_str(b10.decrypt(z0)) == "z0" && "PN: z0 failed");
      assert(b10.skipped_key_count() == 1 &&
             "PN: one key from the old chain must be banked");

      // The delayed old-chain message finally arrives.
      assert(to_str(b10.decrypt(x1)) == "x1" && "PN: late x1 failed");
      assert(b10.skipped_key_count() == 0 && "PN: bank must be empty");

      spdlog::info("Phase 10 OK: cross-chain late delivery handled.");
    }

    // ---- 11. Replay and duplicate rejection ----
    spdlog::info("--- Phase 11: Replay/duplicate rejection ---");
    {
      pqdh::KeyPairs a11_pqdh = pqdh::generate_keypairs();
      pqdh::KeyPairs b11_pqdh = pqdh::generate_keypairs();

      DoubleRatchet a11, b11;
      std::vector<uint8_t> ct11 = a11.pq_init(
          initial_root_key, a11_pqdh, b11_pqdh.dh_pub, b11_pqdh.pq_pk, true);
      b11.pq_init(initial_root_key, b11_pqdh, a11_pqdh.dh_pub, ct11, false);

      EncryptedMessage d0 = a11.encrypt(to_vec("d0"));
      EncryptedMessage d1 = a11.encrypt(to_vec("d1"));
      EncryptedMessage d2 = a11.encrypt(to_vec("d2"));

      bool ok = false;
      assert(to_str(b11.decrypt(d0, &ok)) == "d0" && ok && "Replay: d0 failed");

      // Straight duplicate of an in-order message.
      b11.decrypt(d0, &ok);
      assert(!ok && "Replay: duplicate d0 must be rejected");

      // A banked key must decrypt once and only once.
      assert(to_str(b11.decrypt(d2, &ok)) == "d2" && ok && "Replay: d2 failed");
      assert(to_str(b11.decrypt(d1, &ok)) == "d1" && ok &&
             "Replay: late d1 failed");
      b11.decrypt(d1, &ok);
      assert(!ok && "Replay: replayed d1 must be rejected");

      // State survived all rejects: the next message still decrypts.
      EncryptedMessage d3 = a11.encrypt(to_vec("d3"));
      assert(to_str(b11.decrypt(d3, &ok)) == "d3" && ok &&
             "Replay: d3 after rejects failed");

      spdlog::info("Phase 11 OK: replays and duplicates rejected safely.");
    }

    // ---- 12. Oversized gap and tampered frames are rejected harmlessly ----
    spdlog::info("--- Phase 12: MAX_SKIP guard and tampered frames ---");
    {
      pqdh::KeyPairs a12_pqdh = pqdh::generate_keypairs();
      pqdh::KeyPairs b12_pqdh = pqdh::generate_keypairs();

      DoubleRatchet a12, b12;
      std::vector<uint8_t> ct12 = a12.pq_init(
          initial_root_key, a12_pqdh, b12_pqdh.dh_pub, b12_pqdh.pq_pk, true);
      b12.pq_init(initial_root_key, b12_pqdh, a12_pqdh.dh_pub, ct12, false);

      EncryptedMessage g0 = a12.encrypt(to_vec("g0"));
      EncryptedMessage g1 = a12.encrypt(to_vec("g1"));

      // A frame demanding a gigantic fast-forward must be refused before any
      // key derivation happens.
      EncryptedMessage forged = g0;
      forged.seq_num = 1000000;
      bool ok = false;
      b12.decrypt(forged, &ok);
      assert(!ok && "MAX_SKIP: forged giant seq_num must be rejected");

      // A tampered ciphertext fails AEAD without desyncing the chain.
      EncryptedMessage corrupt = g0;
      if (!corrupt.ciphertext.empty()) corrupt.ciphertext[0] ^= 0xFF;
      b12.decrypt(corrupt, &ok);
      assert(!ok && "Tamper: corrupt ciphertext must be rejected");

      // The genuine messages still decrypt after both attacks.
      assert(to_str(b12.decrypt(g0, &ok)) == "g0" && ok &&
             "Tamper: genuine g0 failed after attacks");
      assert(to_str(b12.decrypt(g1, &ok)) == "g1" && ok &&
             "Tamper: genuine g1 failed after attacks");
      assert(b12.skipped_key_count() == 0 &&
             "Tamper: failed frames must not pollute the bank");

      spdlog::info("Phase 12 OK: hostile frames rejected without damage.");
    }

    // ---- 13. Banked keys survive session save/restore ----
    spdlog::info("--- Phase 13: Skipped keys persistence ---");
    {
      pqdh::KeyPairs a13_pqdh = pqdh::generate_keypairs();
      pqdh::KeyPairs b13_pqdh = pqdh::generate_keypairs();

      DoubleRatchet a13, b13;
      std::vector<uint8_t> ct13 = a13.pq_init(
          initial_root_key, a13_pqdh, b13_pqdh.dh_pub, b13_pqdh.pq_pk, true);
      b13.pq_init(initial_root_key, b13_pqdh, a13_pqdh.dh_pub, ct13, false);

      EncryptedMessage p0 = a13.encrypt(to_vec("p0"));
      EncryptedMessage p1 = a13.encrypt(to_vec("p1"));
      EncryptedMessage p2 = a13.encrypt(to_vec("p2"));

      // p2 arrives first, keys for p0/p1 get banked, then the app "restarts".
      assert(to_str(b13.decrypt(p2)) == "p2" && "Persist: p2 failed");
      assert(b13.skipped_key_count() == 2 && "Persist: expected 2 banked keys");

      nlohmann::json b13_json = b13.to_json();
      DoubleRatchet b13r;
      assert(b13r.from_json(b13_json) && "Persist: restore failed");
      assert(b13r.skipped_key_count() == 2 &&
             "Persist: banked keys lost on restore");

      // The delayed messages decrypt on the restored session.
      bool ok = false;
      assert(to_str(b13r.decrypt(p1, &ok)) == "p1" && ok &&
             "Persist: p1 after restore failed");
      assert(to_str(b13r.decrypt(p0, &ok)) == "p0" && ok &&
             "Persist: p0 after restore failed");

      // And the session keeps working both ways.
      EncryptedMessage p3 = a13.encrypt(to_vec("p3"));
      assert(to_str(b13r.decrypt(p3, &ok)) == "p3" && ok &&
             "Persist: p3 failed");
      EncryptedMessage p4 = b13r.encrypt(to_vec("p4"));
      assert(to_str(a13.decrypt(p4, &ok)) == "p4" && ok &&
             "Persist: p4 failed");

      spdlog::info("Phase 13 OK: banked keys survive save/restore.");
    }

    // ---- 14. Regression: crossing replies must never desync the root key ----
    // Historical bug: a send-side ratchet triggered by "was my last action a
    // receive" (any receive, including a stale/already-known-key one) could
    // discard a local key the peer's in-flight reply depended on, permanently
    // bricking the session in both directions. The fix ties key regeneration
    // strictly to observing a genuinely NEW remote key (inside decrypt()), so
    // a stale receive can never trigger it. This reproduces the exact
    // interleaving that broke the old implementation.
    spdlog::info("--- Phase 14: Crossing-reply race must not desync root key ---");
    {
      pqdh::KeyPairs a14_pqdh = pqdh::generate_keypairs();
      pqdh::KeyPairs b14_pqdh = pqdh::generate_keypairs();

      DoubleRatchet a14, b14;
      std::vector<uint8_t> ct14 = a14.pq_init(
          initial_root_key, a14_pqdh, b14_pqdh.dh_pub, b14_pqdh.pq_pk, true);
      b14.pq_init(initial_root_key, b14_pqdh, a14_pqdh.dh_pub, ct14, false);

      bool ok = false;

      EncryptedMessage A0 = a14.encrypt(to_vec("A0"));
      assert(to_str(b14.decrypt(A0, &ok)) == "A0" && ok && "Race: A0 failed");

      EncryptedMessage B1 = b14.encrypt(to_vec("B1"));  // Bob's bootstrap ratchet
      EncryptedMessage A1 = a14.encrypt(to_vec("A1"));  // still Alice's original key

      assert(to_str(a14.decrypt(B1, &ok)) == "B1" && ok && "Race: B1 failed");
      EncryptedMessage A2 = a14.encrypt(to_vec("A2"));  // Alice's atomic ratchet

      // Stale message: Bob's dh_remote_ already matches it, so this must NOT
      // trigger any local re-key on Bob's side.
      assert(to_str(b14.decrypt(A1, &ok)) == "A1" && ok && "Race: A1 failed");
      EncryptedMessage B2 = b14.encrypt(to_vec("B2"));  // must reuse Bob_keyGen1

      // The critical check: A2 was computed against Bob_keyGen1. If the stale
      // A1 receive above had spuriously discarded it, this decrypt fails and
      // never recovers (matching the historical bug).
      assert(to_str(b14.decrypt(A2, &ok)) == "A2" && ok &&
             "Race: A2 failed — root key desynced");
      assert(to_str(a14.decrypt(B2, &ok)) == "B2" && ok &&
             "Race: B2 failed — root key desynced");

      // Session must keep working normally afterward.
      for (int i = 0; i < 3; i++) {
        EncryptedMessage m = a14.encrypt(to_vec("retry-a-" + std::to_string(i)));
        assert(to_str(b14.decrypt(m, &ok)) == "retry-a-" + std::to_string(i) &&
               ok && "Race: post-recovery a->b failed");
        EncryptedMessage m2 = b14.encrypt(to_vec("retry-b-" + std::to_string(i)));
        assert(to_str(a14.decrypt(m2, &ok)) == "retry-b-" + std::to_string(i) &&
               ok && "Race: post-recovery b->a failed");
      }

      spdlog::info("Phase 14 OK: crossing replies no longer desync the root key.");
    }

    // ---- 15. Security audit fix: undersized handshake fields must throw a
    // clean exception, never read past the buffer they were given. ----
    // Historical bug: pq_init/pqdh_ratchet_alice/pqdh_ratchet_bob copied
    // exactly KEY_LEN bytes out of dh_pub_remote (and OQS_KEM_encaps/decaps
    // read exactly length_public_key/length_ciphertext bytes out of pq_pub /
    // the encapsulated ciphertext) regardless of how many bytes the caller
    // actually supplied. A session_request/prekey with a truncated field was
    // a heap buffer over-read, not a rejected message. This reproduces the
    // exact truncated-field shapes an attacker (or a malicious self-signed
    // prekey — verify_prekey never checked pq_pub's length) could send.
    spdlog::info(
        "--- Phase 15: Truncated handshake fields are rejected cleanly ---");
    {
      pqdh::KeyPairs a15_pqdh = pqdh::generate_keypairs();
      pqdh::KeyPairs b15_pqdh = pqdh::generate_keypairs();

      // Truncated dh_pub_remote (5 bytes instead of 32).
      {
        std::vector<uint8_t> short_dh_pub(5, 0xAA);
        DoubleRatchet victim;
        bool threw = false;
        try {
          victim.pq_init(initial_root_key, a15_pqdh, short_dh_pub,
                         b15_pqdh.pq_pk, true);
        } catch (const std::exception&) {
          threw = true;
        }
        assert(threw &&
               "Truncated dh_pub_remote must throw, not over-read the buffer");
      }

      // Truncated pq_pub_remote (initiator path: encapsulate against it).
      {
        std::vector<uint8_t> short_pq_pub(3, 0xBB);
        DoubleRatchet victim;
        bool threw = false;
        try {
          victim.pq_init(initial_root_key, a15_pqdh, b15_pqdh.dh_pub,
                         short_pq_pub, true);
        } catch (const std::exception&) {
          threw = true;
        }
        assert(threw &&
               "Truncated pq_pub_remote must throw, not over-read the buffer");
      }

      // Truncated pq_ciphertext (responder path: decapsulate it) — this is
      // the exact shape of an unauthenticated, attacker-sent session_request
      // with a hand-shortened encapsulated_ciphertext field.
      {
        std::vector<uint8_t> short_ciphertext(4, 0xCC);
        DoubleRatchet victim;
        bool threw = false;
        try {
          victim.pq_init(initial_root_key, b15_pqdh, a15_pqdh.dh_pub,
                         short_ciphertext, false);
        } catch (const std::exception&) {
          threw = true;
        }
        assert(threw && "Truncated pq_ciphertext must throw, not over-read "
                        "the buffer");
      }

      // Empty fields (the degenerate case — an attacker-sent empty hex
      // string decodes to a zero-length vector).
      {
        std::vector<uint8_t> empty_field;
        DoubleRatchet victim;
        bool threw = false;
        try {
          victim.pq_init(initial_root_key, b15_pqdh, a15_pqdh.dh_pub,
                         empty_field, false);
        } catch (const std::exception&) {
          threw = true;
        }
        assert(threw && "Empty pq_ciphertext must throw, not over-read the "
                        "buffer");
      }

      spdlog::info(
          "Phase 15 OK: truncated/empty handshake fields rejected cleanly, "
          "no over-read.");
    }

    spdlog::info(
        "================= All DH Ratchet Direction Change Tests PASSED "
        "=================");
  }

  return 0;
}