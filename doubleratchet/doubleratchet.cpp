//
// Created by r on 13.10.2025.
//

#include "doubleratchet.h"

#include <openssl/sha.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "corecrypto.h"
#include "kdf.h"
#include "pq.h"
#include "pqdh.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace dr {

using dr::DHKeyPair;
using dr::Key32;
using dr::PubKey;

enum EncryptedMessageError : uint8_t { NONE = 0, ENCRYPTION_FAILED = 1 };

static void zero_and_clear(std::map<MessageUID, Key32>& keys) {
  for (auto& entry : keys) {
    CoreCrypto::secure_memory_zero(entry.second.data(), entry.second.size());
  }
  keys.clear();
}

void DoubleRatchet::debug_print() const {
#ifndef NDEBUG
  spdlog::debug("DoubleRatchet Debug Info:");
  spdlog::debug("Root Key: {}",
                CoreCrypto::to_hex(root_key_.data(), root_key_.size()));
  spdlog::debug(
      "Chain Key Send: {}",
      CoreCrypto::to_hex(chain_key_send_.data(), chain_key_send_.size()));
  spdlog::debug(
      "Chain Key Receive: {}",
      CoreCrypto::to_hex(chain_key_receive_.data(), chain_key_receive_.size()));
  spdlog::debug("DH Local Priv: {}", CoreCrypto::to_hex(dh_local_.priv.data(),
                                                        dh_local_.priv.size()));
  spdlog::debug("DH Local Pub: {}",
                CoreCrypto::to_hex(dh_local_.pub.data(), dh_local_.pub.size()));
  spdlog::debug("DH Remote: {}",
                CoreCrypto::to_hex(dh_remote_.data(), dh_remote_.size()));
  spdlog::debug("UID: {}", CoreCrypto::to_hex(uid_.data(), uid_.size()));
  spdlog::debug("Seq Num Send: {}", seq_num_send_);
  spdlog::debug("Seq Num Recv: {}", seq_num_recv_);
  spdlog::debug("DH Ratchet Step: {}", dh_ratchet_step_);
  spdlog::debug("Prev Chain Len: {}", prev_chain_len_);
  spdlog::debug("Recv Chain Label: {}", recv_chain_label_);
  spdlog::debug("Skipped Keys Banked: {}", skipped_keys_.size());
  spdlog::debug("Pending Initial Send Ratchet: {}",
               pending_initial_send_ratchet_);
#endif
}

const dr::Key32& DoubleRatchet::_debug_root_key() const { return root_key_; }

const dr::Key32& DoubleRatchet::_debug_chain_key_send() const {
  return chain_key_send_;
}

const dr::Key32& DoubleRatchet::_debug_chain_key_receive() const {
  return chain_key_receive_;
}

const std::array<uint8_t, 16>& DoubleRatchet::get_uid() const { return uid_; }

const uint32_t DoubleRatchet::get_seq_num_send() const { return seq_num_send_; }

const uint32_t DoubleRatchet::get_seq_num_recv() const { return seq_num_recv_; }

const uint32_t DoubleRatchet::get_dh_ratchet_step() const {
  return dh_ratchet_step_;
}

void DoubleRatchet::dh_ratchet(const DHKeyPair& new_dh_local,
                               const PubKey& new_dh_remote) {
  dh_local_ = new_dh_local;
  dh_remote_ = new_dh_remote;

  std::vector<uint8_t> my_sk(dh_local_.priv.begin(), dh_local_.priv.end());
  std::vector<uint8_t> their_pk(dh_remote_.begin(), dh_remote_.end());

  Key32 dh_secret_ = CoreCrypto::derive_dh_secret(my_sk, their_pk);

  CoreCrypto::secure_memory_zero(their_pk);

  Key32 new_root_key_{};

  kdf::kdf_root(root_key_, dh_secret_, new_root_key_, chain_key_send_,
                chain_key_receive_);

  CoreCrypto::secure_memory_zero(dh_secret_.data(), dh_secret_.size());
  CoreCrypto::secure_memory_zero(my_sk);

  if (!is_initiator_) {
    std::swap(chain_key_send_, chain_key_receive_);
  }

  root_key_ = new_root_key_;

  CoreCrypto::secure_memory_zero(new_root_key_.data(), new_root_key_.size());

  seq_num_recv_ = 0;
  seq_num_send_ = 0;
  prev_chain_len_ = 0;
  dh_ratchet_step_++;
  recv_chain_label_ = dh_ratchet_step_;
}

// Atomic DH ratchet: fires only when decrypt() observes a remote public key
// we have not seen before. Derives the receive chain from our CURRENT local
// key + their new key, then generates a FRESH local key and derives the send
// chain from it + that same new remote key, chained onto the root key the
// receive half just produced. Both halves are one conceptual event: one root
// derivation cascade, one dh_ratchet_step_ increment.
void DoubleRatchet::advance_to_new_remote_key(const PubKey& new_dh_remote) {
  dh_remote_ = new_dh_remote;

  // --- Receive half: DH(current local priv, new remote pub) ---
  {
    std::vector<uint8_t> my_sk(dh_local_.priv.begin(), dh_local_.priv.end());
    std::vector<uint8_t> their_pk(dh_remote_.begin(), dh_remote_.end());

    Key32 dh_secret = CoreCrypto::derive_dh_secret(my_sk, their_pk);

    CoreCrypto::secure_memory_zero(their_pk);
    CoreCrypto::secure_memory_zero(my_sk);

    Key32 new_root_key{};
    Key32 ck_a{};
    Key32 ck_b{};

    kdf::kdf_root(root_key_, dh_secret, new_root_key, ck_a, ck_b);

    CoreCrypto::secure_memory_zero(dh_secret.data(), dh_secret.size());

    // initiator: recv=ck_b; non-initiator: recv=ck_a
    chain_key_receive_ = is_initiator_ ? ck_b : ck_a;

    root_key_ = new_root_key;

    CoreCrypto::secure_memory_zero(new_root_key.data(), new_root_key.size());
    CoreCrypto::secure_memory_zero(ck_a.data(), ck_a.size());
    CoreCrypto::secure_memory_zero(ck_b.data(), ck_b.size());
  }
  seq_num_recv_ = 0;

  // --- Send half: fresh local keypair, DH(new local priv, same new remote pub) ---
  prev_chain_len_ = seq_num_send_;
  dh_local_ = generate_new_dh_keypair();
  {
    std::vector<uint8_t> my_sk(dh_local_.priv.begin(), dh_local_.priv.end());
    std::vector<uint8_t> their_pk(dh_remote_.begin(), dh_remote_.end());

    Key32 dh_secret = CoreCrypto::derive_dh_secret(my_sk, their_pk);

    CoreCrypto::secure_memory_zero(their_pk);
    CoreCrypto::secure_memory_zero(my_sk);

    Key32 new_root_key{};
    Key32 ck_a{};
    Key32 ck_b{};

    kdf::kdf_root(root_key_, dh_secret, new_root_key, ck_a, ck_b);

    CoreCrypto::secure_memory_zero(dh_secret.data(), dh_secret.size());

    // initiator: send=ck_a; non-initiator: send=ck_b
    chain_key_send_ = is_initiator_ ? ck_a : ck_b;

    root_key_ = new_root_key;

    CoreCrypto::secure_memory_zero(new_root_key.data(), new_root_key.size());
    CoreCrypto::secure_memory_zero(ck_a.data(), ck_a.size());
    CoreCrypto::secure_memory_zero(ck_b.data(), ck_b.size());
  }
  seq_num_send_ = 0;

  dh_ratchet_step_++;
  // recv_chain_label_ is not set here: decrypt() always overwrites it with the
  // sender's own stamped dh_ratchet_step right after this call returns (that
  // is the correct label for future skipped-key lookups, since it is what the
  // sender will keep stamping on this chain — our own combined-ratchet
  // counter is a different, purely local tally).
}

// One-shot bootstrap send-ratchet (see pending_initial_send_ratchet_ in the
// header for why this is needed). Generates a new DH keypair, derives a new
// sending chain key combined with the CURRENT known remote key. Updates only
// chain_key_send_ and root_key_ — the receive chain is untouched, since the
// remote key hasn't actually changed from our perspective yet.
void DoubleRatchet::dh_ratchet_send() {
  dh_local_ = generate_new_dh_keypair();

  std::vector<uint8_t> my_sk(dh_local_.priv.begin(), dh_local_.priv.end());
  std::vector<uint8_t> their_pk(dh_remote_.begin(), dh_remote_.end());

  Key32 dh_secret = CoreCrypto::derive_dh_secret(my_sk, their_pk);

  CoreCrypto::secure_memory_zero(their_pk);
  CoreCrypto::secure_memory_zero(my_sk);

  Key32 new_root_key{};
  Key32 ck_a{};
  Key32 ck_b{};

  kdf::kdf_root(root_key_, dh_secret, new_root_key, ck_a, ck_b);

  CoreCrypto::secure_memory_zero(dh_secret.data(), dh_secret.size());

  // initiator: send=ck_a; non-initiator: send=ck_b
  if (is_initiator_) {
    chain_key_send_ = ck_a;
  } else {
    chain_key_send_ = ck_b;
  }

  root_key_ = new_root_key;

  CoreCrypto::secure_memory_zero(new_root_key.data(), new_root_key.size());
  CoreCrypto::secure_memory_zero(ck_a.data(), ck_a.size());
  CoreCrypto::secure_memory_zero(ck_b.data(), ck_b.size());

  prev_chain_len_ = seq_num_send_;
  seq_num_send_ = 0;
  dh_ratchet_step_++;
}

std::vector<uint8_t> DoubleRatchet::pqdh_ratchet_alice(
    const pqdh::KeyPairs& pqdh_local, const std::vector<uint8_t>& dh_pub_remote,
    const std::vector<uint8_t>& pq_pub_remote) {
  // dh_pub_remote comes from the peer's prekey bundle; std::copy_n below
  // reads exactly KEY_LEN bytes regardless of the vector's real size, so an
  // undersized value here would be a heap buffer over-read, not a clean
  // error.
  if (dh_pub_remote.size() != KEY_LEN) {
    throw std::runtime_error("pqdh_ratchet_alice: invalid dh_pub_remote length");
  }

  std::copy_n(pqdh_local.dh_pub.begin(), KEY_LEN, dh_local_.pub.begin());
  std::copy_n(pqdh_local.dh_priv.begin(), KEY_LEN, dh_local_.priv.begin());

  std::array<uint8_t, KEY_LEN> dh_pub_remote_array{};
  std::copy_n(dh_pub_remote.data(), KEY_LEN, dh_pub_remote_array.data());

  dh_remote_ = dh_pub_remote_array;

  std::vector<uint8_t> my_sk(dh_local_.priv.begin(), dh_local_.priv.end());
  std::vector<uint8_t> their_pk(dh_remote_.begin(), dh_remote_.end());

  pq::Encapsulated local_encapsulated = pqdh::make_encapsulated(pq_pub_remote);

  Key32 dh_secret_ = CoreCrypto::derive_dh_secret(my_sk, their_pk);

  std::vector<uint8_t> pqdh_shared_key = pqdh::make_shared_key(
      local_encapsulated.shared_secret,
      std::vector<uint8_t>(dh_secret_.begin(), dh_secret_.end()));

  Key32 new_root_key_{};

  std::array<uint8_t, KEY_LEN> pqdh_shared_key_array{};

  std::copy_n(pqdh_shared_key.data(), KEY_LEN, pqdh_shared_key_array.data());

  kdf::kdf_root(root_key_, pqdh_shared_key_array, new_root_key_,
                chain_key_send_, chain_key_receive_);

  CoreCrypto::secure_memory_zero(dh_secret_.data(), dh_secret_.size());
  CoreCrypto::secure_memory_zero(my_sk);
  CoreCrypto::secure_memory_zero(their_pk);
  CoreCrypto::secure_memory_zero(pqdh_shared_key.data(),
                                 pqdh_shared_key.size());
  CoreCrypto::secure_memory_zero(pqdh_shared_key_array.data(),
                                 pqdh_shared_key_array.size());
  CoreCrypto::secure_memory_zero(local_encapsulated.shared_secret);

  if (!is_initiator_) {
    std::swap(chain_key_send_, chain_key_receive_);
  }

  root_key_ = new_root_key_;

  CoreCrypto::secure_memory_zero(new_root_key_.data(), new_root_key_.size());

  seq_num_recv_ = 0;
  seq_num_send_ = 0;
  prev_chain_len_ = 0;
  dh_ratchet_step_++;
  recv_chain_label_ = dh_ratchet_step_;

  return local_encapsulated.ciphertext;
}

void DoubleRatchet::pqdh_ratchet_bob(
    const pqdh::KeyPairs& pqdh_local, const std::vector<uint8_t>& dh_pub_remote,
    const std::vector<uint8_t>& pq_ciphertext) {
  // dh_pub_remote here comes straight off an incoming, unauthenticated
  // session_request — fully attacker-controlled. Same over-read risk as in
  // pqdh_ratchet_alice.
  if (dh_pub_remote.size() != KEY_LEN) {
    throw std::runtime_error("pqdh_ratchet_bob: invalid dh_pub_remote length");
  }

  std::copy_n(pqdh_local.dh_priv.begin(), KEY_LEN, dh_local_.priv.begin());
  std::copy_n(pqdh_local.dh_pub.begin(), KEY_LEN, dh_local_.pub.begin());

  std::array<uint8_t, KEY_LEN> dh_pub_remote_array{};
  std::copy_n(dh_pub_remote.data(), KEY_LEN, dh_pub_remote_array.data());

  dh_remote_ = dh_pub_remote_array;

  std::vector<uint8_t> my_sk(dh_local_.priv.begin(), dh_local_.priv.end());
  std::vector<uint8_t> their_pk(dh_remote_.begin(), dh_remote_.end());

  pqdh::SharedSecrets shared_secrets = pqdh::derive_shared_secrets(
      pq_ciphertext, pqdh_local.pq_sk, my_sk, their_pk);

  std::vector<uint8_t> pqdh_shared_key = pqdh::make_shared_key(
      shared_secrets.pq_shared_secret, shared_secrets.dh_shared_secret);

  Key32 new_root_key_{};

  std::array<uint8_t, KEY_LEN> pqdh_shared_key_array{};

  std::copy_n(pqdh_shared_key.data(), KEY_LEN, pqdh_shared_key_array.data());

  kdf::kdf_root(root_key_, pqdh_shared_key_array, new_root_key_,
                chain_key_send_, chain_key_receive_);

  CoreCrypto::secure_memory_zero(my_sk);
  CoreCrypto::secure_memory_zero(pqdh_shared_key.data(),
                                 pqdh_shared_key.size());
  CoreCrypto::secure_memory_zero(pqdh_shared_key_array.data(),
                                 pqdh_shared_key_array.size());

  CoreCrypto::secure_memory_zero(shared_secrets.pq_shared_secret);
  CoreCrypto::secure_memory_zero(shared_secrets.dh_shared_secret);

  CoreCrypto::secure_memory_zero(their_pk);

  if (!is_initiator_) {
    std::swap(chain_key_send_, chain_key_receive_);
  }

  root_key_ = new_root_key_;

  CoreCrypto::secure_memory_zero(new_root_key_.data(), new_root_key_.size());

  seq_num_recv_ = 0;
  seq_num_send_ = 0;
  prev_chain_len_ = 0;
  dh_ratchet_step_++;
  recv_chain_label_ = dh_ratchet_step_;
}

DoubleRatchet& DoubleRatchet::init(const Key32& initial_root_key,
                                   const DHKeyPair& dh_local,
                                   const PubKey& dh_pub_remote,
                                   const bool is_initiator) {
  root_key_ = initial_root_key;
  is_initiator_ = is_initiator;

  zero_and_clear(skipped_keys_);  // fresh session — old banked keys are void

  uid_ = generate_uid(root_key_, dh_local.pub, dh_pub_remote);
  dh_ratchet(dh_local, dh_pub_remote);

  pending_initial_send_ratchet_ = !is_initiator_;
  error_code = 0;

  return *this;
}

std::vector<uint8_t> DoubleRatchet::pq_init(
    const Key32& initial_root_key, const pqdh::KeyPairs& pqdh_local,
    const std::vector<uint8_t>& dh_pub_remote,
    const std::vector<uint8_t>& pq_pub_remote_or_ciphertext,
    const bool is_initiator) {
  root_key_ = initial_root_key;
  is_initiator_ = is_initiator;

  zero_and_clear(skipped_keys_);  // fresh session — old banked keys are void

  // dh_pub_remote is attacker-controlled on the responder path (an incoming
  // session_request) and only weakly validated on the initiator path (a
  // prekey bundle whose signer controls every field). std::copy_n below
  // reads exactly KEY_LEN bytes regardless of the vector's real size, so
  // check up front rather than risk a heap buffer over-read.
  if (dh_pub_remote.size() != KEY_LEN) {
    throw std::runtime_error("pq_init: invalid dh_pub_remote length");
  }

  std::array<uint8_t, KEY_LEN> dh_pub_local_array{};
  std::copy_n(pqdh_local.dh_pub.data(), KEY_LEN, dh_pub_local_array.data());

  std::array<uint8_t, KEY_LEN> dh_pub_remote_array{};
  std::copy_n(dh_pub_remote.data(), KEY_LEN, dh_pub_remote_array.data());

  uid_ = generate_uid(root_key_, dh_pub_local_array, dh_pub_remote_array);

  pending_initial_send_ratchet_ = !is_initiator_;
  error_code = 0;

  if (is_initiator_) {
    return pqdh_ratchet_alice(pqdh_local, dh_pub_remote,
                              pq_pub_remote_or_ciphertext);
  } else {
    pqdh_ratchet_bob(pqdh_local, dh_pub_remote, pq_pub_remote_or_ciphertext);
    return std::vector<uint8_t>{};
  }
}

void DoubleRatchet::ratchet_send(Key32& message_key_send) {
  kdf::kdf_chain_send(chain_key_send_, message_key_send);
  seq_num_send_++;
}

void DoubleRatchet::ratchet_receive(Key32& message_key_receive) {
  kdf::kdf_chain_receive(chain_key_receive_, message_key_receive);
  seq_num_recv_++;
}

MessageUID DoubleRatchet::get_recv_message_uid() const {
  MessageUID uid;
  uid.uid = this->uid_;
  // recv_chain_label_, not dh_ratchet_step_: this must match the step value
  // the SENDER stamps on messages of our current receive chain. dh_ratchet_step_
  // is our own local tally of atomic ratchet events and is not guaranteed to
  // equal the peer's — see advance_to_new_remote_key.
  uid.dh_ratchet_step = this->recv_chain_label_;
  uid.seq_num = this->seq_num_recv_;
  return uid;
}

MessageUID DoubleRatchet::get_send_message_uid() const {
  MessageUID uid;
  uid.uid = this->uid_;
  uid.dh_ratchet_step = this->dh_ratchet_step_;
  uid.seq_num = this->seq_num_send_;
  return uid;
}

const std::string DoubleRatchet::get_session_key_fingerprint() const {
  std::vector<uint8_t> data;

  const std::string context = "DoubleRatchetSessionKeyFingerprint|v1.0.0";
  data.insert(data.end(), context.begin(), context.end());

  data.insert(data.end(), root_key_.begin(), root_key_.end());

  if (is_initiator_) {
    data.insert(data.end(), dh_local_.pub.begin(), dh_local_.pub.end());
    data.insert(data.end(), dh_remote_.begin(), dh_remote_.end());
  } else {
    data.insert(data.end(), dh_remote_.begin(), dh_remote_.end());
    data.insert(data.end(), dh_local_.pub.begin(), dh_local_.pub.end());
  }

  std::array<uint8_t, 32> fingerprint;
  SHA256(data.data(), data.size(), fingerprint.data());

  CoreCrypto::secure_memory_zero(data);

  // Convert the hash to a numeric value and format it as
  // 0000-0000-0000-0000-0000
  uint64_t hash_value = 0;
  for (size_t i = 0; i < 8; ++i) {
    hash_value = (hash_value << 8) | fingerprint[i];
  }

  // Keep only the first 20 decimal digits
  std::string digits = std::to_string(hash_value);
  if (digits.length() < 20) {
    digits = std::string(20 - digits.length(), '0') + digits;
  } else if (digits.length() > 20) {
    digits = digits.substr(0, 20);
  }

  // Format into groups of 4 digits separated by dashes
  std::string result;
  result.reserve(24);  // 20 digits + 4 dashes
  for (size_t i = 0; i < 20; i += 4) {
    if (i > 0) {
      result += '-';
    }
    result += digits.substr(i, 4);
  }

  return result;
}

DoubleRatchet::~DoubleRatchet() {
  CoreCrypto::secure_memory_zero(root_key_.data(), root_key_.size());
  CoreCrypto::secure_memory_zero(chain_key_send_.data(),
                                 chain_key_send_.size());
  CoreCrypto::secure_memory_zero(chain_key_receive_.data(),
                                 chain_key_receive_.size());
  CoreCrypto::secure_memory_zero(dh_local_.priv.data(), dh_local_.priv.size());
  CoreCrypto::secure_memory_zero(dh_local_.pub.data(), dh_local_.pub.size());
  CoreCrypto::secure_memory_zero(dh_remote_.data(), dh_remote_.size());
  CoreCrypto::secure_memory_zero(uid_.data(), uid_.size());
  zero_and_clear(skipped_keys_);
  seq_num_send_ = 0;
  seq_num_recv_ = 0;
  dh_ratchet_step_ = 0;
  prev_chain_len_ = 0;
  recv_chain_label_ = 0;
  is_initiator_ = false;
  pending_initial_send_ratchet_ = false;
}

std::array<uint8_t, 16> DoubleRatchet::generate_uid(
    const dr::Key32& root_key, const dr::PubKey& local_pub,
    const dr::PubKey& remote_pub) {
  std::vector<uint8_t> data;

  const std::string context = "DoubleRatchetUID|v1.0.0";
  data.insert(data.end(), context.begin(), context.end());

  data.insert(data.end(), root_key.begin(), root_key.end());

  if (is_initiator_) {
    data.insert(data.end(), local_pub.begin(), local_pub.end());
    data.insert(data.end(), remote_pub.begin(), remote_pub.end());
  } else {
    data.insert(data.end(), remote_pub.begin(), remote_pub.end());
    data.insert(data.end(), local_pub.begin(), local_pub.end());
  }

  std::array<uint8_t, 32> uid;
  SHA256(data.data(), data.size(), uid.data());

  CoreCrypto::secure_memory_zero(data);

  std::array<uint8_t, 16> short_uid{};
  std::copy_n(uid.data(), 16, short_uid.data());

  CoreCrypto::secure_memory_zero(uid.data(), uid.size());

  return short_uid;
}

nlohmann::json DoubleRatchet::to_json() const {
  nlohmann::json j;
  j["root_key"] = std::vector<uint8_t>(root_key_.begin(), root_key_.end());
  j["dh_local_priv"] =
      std::vector<uint8_t>(dh_local_.priv.begin(), dh_local_.priv.end());
  j["dh_local_pub"] =
      std::vector<uint8_t>(dh_local_.pub.begin(), dh_local_.pub.end());
  j["dh_remote"] = std::vector<uint8_t>(dh_remote_.begin(), dh_remote_.end());
  j["chain_key_send"] =
      std::vector<uint8_t>(chain_key_send_.begin(), chain_key_send_.end());
  j["chain_key_receive"] = std::vector<uint8_t>(chain_key_receive_.begin(),
                                                chain_key_receive_.end());
  j["seq_num_send"] = seq_num_send_;
  j["seq_num_recv"] = seq_num_recv_;
  j["dh_ratchet_step"] = dh_ratchet_step_;
  j["prev_chain_len"] = prev_chain_len_;
  j["recv_chain_label"] = recv_chain_label_;
  j["uid"] = std::vector<uint8_t>(uid_.begin(), uid_.end());
  j["is_initiator"] = is_initiator_;
  j["pending_initial_send_ratchet"] = pending_initial_send_ratchet_;
  j["error_code"] = error_code;

  // Banked out-of-order keys must survive a restart, or every gap open at
  // shutdown becomes permanently undecryptable.
  nlohmann::json skipped = nlohmann::json::array();
  for (const auto& [slot, key] : skipped_keys_) {
    nlohmann::json entry;
    entry["step"] = slot.dh_ratchet_step;
    entry["seq"] = slot.seq_num;
    entry["key"] = std::vector<uint8_t>(key.begin(), key.end());
    skipped.push_back(entry);
  }
  j["skipped_keys"] = skipped;

  return j;
}

bool DoubleRatchet::from_json(const nlohmann::json& j) {
  try {
    auto vec_to_arr = [](const std::vector<uint8_t>& v, Key32& arr) {
      if (v.size() != KEY_LEN) return false;
      std::copy(v.begin(), v.end(), arr.begin());
      return true;
    };
    auto vec_to_arr16 = [](const std::vector<uint8_t>& v,
                           std::array<uint8_t, 16>& arr) {
      if (v.size() != arr.size()) return false;
      std::copy(v.begin(), v.end(), arr.begin());
      return true;
    };
    if (!vec_to_arr(j.at("root_key").get<std::vector<uint8_t>>(), root_key_))
      return false;
    if (!vec_to_arr(j.at("dh_local_priv").get<std::vector<uint8_t>>(),
                    dh_local_.priv))
      return false;
    if (!vec_to_arr(j.at("dh_local_pub").get<std::vector<uint8_t>>(),
                    dh_local_.pub))
      return false;
    if (!vec_to_arr(j.at("dh_remote").get<std::vector<uint8_t>>(), dh_remote_))
      return false;
    if (!vec_to_arr(j.at("chain_key_send").get<std::vector<uint8_t>>(),
                    chain_key_send_))
      return false;
    if (!vec_to_arr(j.at("chain_key_receive").get<std::vector<uint8_t>>(),
                    chain_key_receive_))
      return false;
    if (!vec_to_arr16(j.at("uid").get<std::vector<uint8_t>>(), uid_))
      return false;
    seq_num_send_ = j.at("seq_num_send").get<uint32_t>();
    seq_num_recv_ = j.at("seq_num_recv").get<uint32_t>();
    dh_ratchet_step_ = j.at("dh_ratchet_step").get<uint32_t>();
    is_initiator_ = j.at("is_initiator").get<bool>();
    // Defaults to false ("already consumed") for sessions saved before this
    // flag existed — by then any real conversation is long past its bootstrap
    // ratchet anyway, so the worst case is a skipped one-time re-key, not a
    // correctness issue.
    pending_initial_send_ratchet_ =
        j.value("pending_initial_send_ratchet", false);
    error_code = j.at("error_code").get<uint8_t>();

    // Fields below were added later; default them so sessions saved by older
    // builds still load. recv_chain_label falls back to the local step — for a
    // session that old the label is unknown, and this merely means one
    // old-chain bank may mislabel (same as the pre-skipped-keys behavior).
    prev_chain_len_ = j.value("prev_chain_len", 0u);
    recv_chain_label_ = j.value("recv_chain_label", dh_ratchet_step_);

    zero_and_clear(skipped_keys_);
    if (j.contains("skipped_keys")) {
      for (const auto& entry : j.at("skipped_keys")) {
        MessageUID slot;
        slot.uid = uid_;
        slot.dh_ratchet_step = entry.at("step").get<uint32_t>();
        slot.seq_num = entry.at("seq").get<uint32_t>();
        Key32 key{};
        if (!vec_to_arr(entry.at("key").get<std::vector<uint8_t>>(), key))
          return false;
        skipped_keys_[slot] = key;
        CoreCrypto::secure_memory_zero(key.data(), key.size());
      }
    }

    return true;
  } catch (...) {
    return false;
  }
}

std::vector<uint8_t> EncryptedMessage::serialize() const {
  std::vector<uint8_t> data;
  data.reserve(1 + 16 + 4 + 4 + 4 + 32 + 24 + 4 + ciphertext.size());

  data.push_back(WIRE_VERSION);  // Wire format version (1 byte)

  data.insert(data.end(), uid.begin(), uid.end());  // UID (16 bytes)

  uint32_t step_be = htonl(dh_ratchet_step);  // DH ratchet step (4 bytes)
  data.insert(data.end(), reinterpret_cast<const uint8_t*>(&step_be),
              reinterpret_cast<const uint8_t*>(&step_be) + 4);

  uint32_t seq_be = htonl(seq_num);  // Sequence number (4 bytes)
  data.insert(data.end(), reinterpret_cast<const uint8_t*>(&seq_be),
              reinterpret_cast<const uint8_t*>(&seq_be) + 4);

  uint32_t pn_be = htonl(prev_chain_len);  // Previous chain length (4 bytes)
  data.insert(data.end(), reinterpret_cast<const uint8_t*>(&pn_be),
              reinterpret_cast<const uint8_t*>(&pn_be) + 4);

  data.insert(data.end(), dh_pub.begin(),
              dh_pub.end());  // DH public key (32 bytes)

  data.insert(data.end(), nonce.begin(), nonce.end());  // Nonce (24 bytes)

  uint32_t ct_len = htonl(
      static_cast<uint32_t>(ciphertext.size()));  // Ciphertext length (4 bytes)
  data.insert(data.end(), reinterpret_cast<const uint8_t*>(&ct_len),
              reinterpret_cast<const uint8_t*>(&ct_len) + 4);

  data.insert(data.end(), ciphertext.begin(), ciphertext.end());  // Ciphertext

  return data;
}

EncryptedMessage EncryptedMessage::deserialize(
    const std::vector<uint8_t>& data) {
  if (data.size() < 1 + 16 + 4 + 4 + 4 + 32 + 24 + 4) {
    throw std::runtime_error("Data too short for EncryptedMessage");
  }

  if (data[0] != WIRE_VERSION) {
    throw std::runtime_error("Unsupported EncryptedMessage wire version");
  }

  EncryptedMessage msg;
  size_t offset = 1;

  std::copy_n(data.begin() + offset, 16, msg.uid.begin());  // UID
  offset += 16;

  uint32_t step_be;  // DH ratchet step
  std::memcpy(&step_be, data.data() + offset, 4);
  msg.dh_ratchet_step = ntohl(step_be);
  offset += 4;

  uint32_t seq_be;  // Sequence number
  std::memcpy(&seq_be, data.data() + offset, 4);
  msg.seq_num = ntohl(seq_be);
  offset += 4;

  uint32_t pn_be;  // Previous chain length
  std::memcpy(&pn_be, data.data() + offset, 4);
  msg.prev_chain_len = ntohl(pn_be);
  offset += 4;

  std::copy_n(data.begin() + offset, 32, msg.dh_pub.begin());  // DH public key
  offset += 32;

  std::copy_n(data.begin() + offset, 24, msg.nonce.begin());  // Nonce
  offset += 24;

  uint32_t ct_len;  // Ciphertext length
  std::memcpy(&ct_len, data.data() + offset, 4);
  ct_len = ntohl(ct_len);
  offset += 4;

  if (offset + ct_len != data.size()) {  // Validate the length
    throw std::runtime_error("Invalid ciphertext length");
  }

  msg.ciphertext.assign(data.begin() + offset, data.end());  // Ciphertext

  return msg;
}

static std::vector<uint8_t> make_associated_data(const EncryptedMessage& msg) {
  std::vector<uint8_t> ad;
  ad.reserve(1 + 16 + 4 + 4 + 4 + 32 + 24);

  ad.push_back(EncryptedMessage::WIRE_VERSION);  // Wire format version

  ad.insert(ad.end(), msg.uid.begin(), msg.uid.end());  // UID

  uint32_t step_be =
      htonl(msg.dh_ratchet_step);  // DH ratchet step (big-endian)
  ad.insert(ad.end(), reinterpret_cast<const uint8_t*>(&step_be),
            reinterpret_cast<const uint8_t*>(&step_be) + 4);

  uint32_t seq_be = htonl(msg.seq_num);  // Sequence number (big-endian)
  ad.insert(ad.end(), reinterpret_cast<const uint8_t*>(&seq_be),
            reinterpret_cast<const uint8_t*>(&seq_be) + 4);

  uint32_t pn_be = htonl(msg.prev_chain_len);  // Previous chain length
  ad.insert(ad.end(), reinterpret_cast<const uint8_t*>(&pn_be),
            reinterpret_cast<const uint8_t*>(&pn_be) + 4);

  ad.insert(ad.end(), msg.dh_pub.begin(), msg.dh_pub.end());  // DH public key

  ad.insert(ad.end(), msg.nonce.begin(), msg.nonce.end());  // Nonce

  return ad;
}

DHKeyPair DoubleRatchet::generate_new_dh_keypair() {
  auto core_dh = CoreCrypto::generate_dh_keypair();
  DHKeyPair kp;
  std::copy_n(core_dh.pubkey.data(), KEY_LEN, kp.pub.begin());
  std::copy_n(core_dh.privkey.data(), KEY_LEN, kp.priv.begin());
  CoreCrypto::secure_memory_zero(core_dh.privkey);
  CoreCrypto::secure_memory_zero(core_dh.pubkey);
  return kp;
}

EncryptedMessage DoubleRatchet::encrypt(const std::vector<uint8_t>& plaintext) {
  // One-shot bootstrap re-key (see pending_initial_send_ratchet_). Every
  // other call just uses whatever dh_local_/chain_key_send_ currently are —
  // decrypt() already keeps them current, atomically, the moment a new
  // remote key is observed. No per-call ratchet decision is made here.
  if (pending_initial_send_ratchet_) {
    dh_ratchet_send();
    pending_initial_send_ratchet_ = false;
  }

  Key32 message_key;
  ratchet_send(message_key);

  // making metadata
  EncryptedMessage msg;
  msg.uid = this->uid_;
  msg.dh_ratchet_step = this->dh_ratchet_step_;
  msg.seq_num = this->seq_num_send_ - 1;  // ratchet_send already incremented it
  msg.prev_chain_len = this->prev_chain_len_;
  msg.dh_pub = dh_local_.pub;

  msg.nonce = CoreCrypto::random<24>();  // gen nonce

  std::vector<uint8_t> ad = make_associated_data(msg);  // making AD

  msg.ciphertext = CoreCrypto::encrypt(
      plaintext, std::vector<uint8_t>(message_key.begin(), message_key.end()),
      std::vector<uint8_t>(msg.nonce.begin(), msg.nonce.end()), ad, false);

  CoreCrypto::secure_memory_zero(message_key.data(), message_key.size());

  if (msg.ciphertext.empty()) {
    msg.error_code = EncryptedMessageError::ENCRYPTION_FAILED;
  }

  return msg;
}

bool DoubleRatchet::skip_message_keys(uint32_t until_seq,
                                      uint32_t chain_label) {
  if (until_seq <= seq_num_recv_) {
    return true;  // nothing to skip
  }

  if (until_seq - seq_num_recv_ > MAX_SKIP) {
    spdlog::error(
        "decrypt: gap of {} messages exceeds MAX_SKIP={}, refusing to "
        "fast-forward the receive chain",
        until_seq - seq_num_recv_, MAX_SKIP);
    return false;
  }

  spdlog::warn("decrypt: banking skipped message keys {}..{} of chain {}",
               seq_num_recv_, until_seq - 1, chain_label);

  while (seq_num_recv_ < until_seq) {
    MessageUID slot;
    slot.uid = uid_;
    slot.dh_ratchet_step = chain_label;
    slot.seq_num = seq_num_recv_;

    Key32 message_key;
    ratchet_receive(message_key);  // advances seq_num_recv_
    skipped_keys_[slot] = message_key;
    CoreCrypto::secure_memory_zero(message_key.data(), message_key.size());
  }

  return true;
}

void DoubleRatchet::prune_skipped_keys() {
  // Map order is (step, seq) ascending and the peer's step never decreases,
  // so begin() is always the stalest entry.
  while (skipped_keys_.size() > MAX_SKIPPED_KEYS) {
    auto oldest = skipped_keys_.begin();
    CoreCrypto::secure_memory_zero(oldest->second.data(),
                                   oldest->second.size());
    skipped_keys_.erase(oldest);
  }
}

std::vector<uint8_t> DoubleRatchet::decrypt(const EncryptedMessage& msg,
                                            bool* ok) {
  if (ok) *ok = false;

  if (msg.uid != this->uid_) {
    spdlog::error("decrypt: message UID mismatch, dropping");
    return {};
  }

  std::vector<uint8_t> ad = make_associated_data(msg);

  // Out-of-order path: the key for this exact (chain, position) was banked
  // when the receive chain fast-forwarded past a gap. Decrypt without touching
  // any chain state; the key is destroyed on success, so a replay of the same
  // frame can never decrypt twice.
  MessageUID slot{msg.uid, msg.dh_ratchet_step, msg.seq_num};
  if (auto banked = skipped_keys_.find(slot); banked != skipped_keys_.end()) {
    std::vector<uint8_t> plaintext;
    try {
      plaintext = CoreCrypto::decrypt(
          msg.ciphertext,
          std::vector<uint8_t>(banked->second.begin(), banked->second.end()),
          std::vector<uint8_t>(msg.nonce.begin(), msg.nonce.end()), ad, true);
    } catch (const std::exception& e) {
      // A forged frame aimed at a banked slot: keep the key, the genuine
      // message may still arrive.
      spdlog::warn(
          "decrypt: AEAD failed for banked slot (chain {}, seq {}), key kept: "
          "{}",
          msg.dh_ratchet_step, msg.seq_num, e.what());
      return {};
    }
    spdlog::info("decrypt: late message (chain {}, seq {}) recovered via "
                 "skipped key",
                 msg.dh_ratchet_step, msg.seq_num);
    CoreCrypto::secure_memory_zero(banked->second.data(),
                                   banked->second.size());
    skipped_keys_.erase(banked);
    if (ok) *ok = true;
    return plaintext;
  }

  // Commit-after-auth: run every ratchet step on a copy of the state and adopt
  // it only once AEAD confirms the message. A forged/replayed/corrupt frame
  // then leaves the real ratchet untouched, so the session survives it.
  // Skipped keys banked below live in the trial copy too, so they are only
  // persisted when the triggering message authenticates.
  DoubleRatchet trial = *this;

  // Atomic DH ratchet if we detect a new remote DH public key. This is the
  // ONLY trigger for regenerating our own send key — never a locally-tracked
  // "direction" flag, which is what let a stale, already-known-key message
  // spuriously discard a key the peer's in-flight reply depended on.
  if (msg.dh_pub != trial.dh_remote_) {
    spdlog::debug("New remote DH pub key detected, performing atomic DH ratchet");
    // The peer moved to a new sending chain. PN says how many messages the
    // current (about-to-be-old) receive chain had in total — bank the keys it
    // still owes us before ratcheting away, or its late messages are lost.
    if (!trial.skip_message_keys(msg.prev_chain_len, trial.recv_chain_label_)) {
      return {};
    }
    trial.advance_to_new_remote_key(msg.dh_pub);
  }

  if (msg.seq_num < trial.seq_num_recv_) {
    // Position already consumed and its key is not banked: a duplicate or a
    // replay. The key material no longer exists, don't waste an AEAD attempt.
    spdlog::warn("decrypt: duplicate or replayed message (chain {}, seq {}), "
                 "dropping",
                 msg.dh_ratchet_step, msg.seq_num);
    return {};
  }

  // Fast-forward over a gap inside the current chain, banking the skipped
  // keys so the delayed messages still decrypt when they arrive.
  if (!trial.skip_message_keys(msg.seq_num, msg.dh_ratchet_step)) {
    return {};
  }

  // 2. Derive the key
  Key32 message_key;
  trial.ratchet_receive(message_key);

  // 3. Decrypt. exceptions=true so an AEAD failure is unambiguous: a valid
  // empty plaintext returns {} without throwing (success), whereas an auth
  // failure throws and we keep the ratchet state unchanged.
  std::vector<uint8_t> plaintext;
  bool success = false;
  try {
    plaintext = CoreCrypto::decrypt(
        msg.ciphertext,
        std::vector<uint8_t>(message_key.begin(), message_key.end()),
        std::vector<uint8_t>(msg.nonce.begin(), msg.nonce.end()), ad, true);
    success = true;
  } catch (const std::exception& e) {
    spdlog::warn("decrypt: AEAD auth failed, ratchet state kept unchanged: {}",
                 e.what());
  }

  CoreCrypto::secure_memory_zero(message_key.data(), message_key.size());

  if (!success) {
    return {};  // do NOT commit trial — real ratchet stays in sync
  }

  // 4. Commit: adopt the advanced state only now that AEAD has authenticated.
  trial.recv_chain_label_ = msg.dh_ratchet_step;
  trial.prune_skipped_keys();
  *this = std::move(trial);
  if (ok) *ok = true;
  return plaintext;
}

}  // namespace dr
