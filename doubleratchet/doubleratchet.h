//
// Created by r on 13.10.2025.
//

#pragma once

#include <openssl/sha.h>

#include <array>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "corecrypto.h"
#include "coreutils.h"
#include "kdf.h"
#include "pq.h"
#include "pqdh.h"

namespace dr {
constexpr size_t KEY_LEN = 32;
using Key32 = std::array<uint8_t, KEY_LEN>;
using PubKey = Key32;
using PrivKey = Key32;

struct DHKeyPair {
  PrivKey priv;
  PubKey pub;
};

struct MessageUID {
  std::array<uint8_t, 16> uid;
  uint32_t dh_ratchet_step;
  uint32_t seq_num;

  bool operator==(const MessageUID& other) const {
    return uid == other.uid && dh_ratchet_step == other.dh_ratchet_step &&
           seq_num == other.seq_num;
  }

  bool operator!=(const MessageUID& other) const { return !(*this == other); }

  bool operator<(const MessageUID& other) const {
    if (dh_ratchet_step < other.dh_ratchet_step) return true;
    if (dh_ratchet_step > other.dh_ratchet_step) return false;
    return seq_num < other.seq_num;
  }

  bool operator>(const MessageUID& other) const {
    if (dh_ratchet_step > other.dh_ratchet_step) return true;
    if (dh_ratchet_step < other.dh_ratchet_step) return false;
    return seq_num > other.seq_num;
  }

  friend std::ostream& operator<<(std::ostream& os, const MessageUID& m) {
    os << "uid=" << coreutils::bytes_to_hex(m.uid.data(), m.uid.size())
       << std::setw(8) << std::setfill('0') << m.dh_ratchet_step << std::setw(8)
       << std::setfill('0') << m.seq_num;
    return os;
  }
};

struct EncryptedMessage {
  // Bumped whenever the wire layout changes; deserialize() rejects frames
  // with a different version instead of misreading them as key material.
  static constexpr uint8_t WIRE_VERSION = 2;

  std::array<uint8_t, 16> uid;
  uint32_t dh_ratchet_step;
  uint32_t seq_num;
  // PN: how many messages the sender's *previous* sending chain contained.
  // Lets the receiver bank keys still owed by the old chain before ratcheting.
  uint32_t prev_chain_len{0};
  PubKey dh_pub{};
  std::array<uint8_t, 24> nonce;
  std::vector<uint8_t> ciphertext;

  uint8_t error_code{0};

  std::vector<uint8_t> serialize() const;
  static EncryptedMessage deserialize(const std::vector<uint8_t>& data);
};

enum class EncryptedMessageFormat : uint8_t {
  BINARY = 0,
  JSON = 1,
  NOT_ENCRYPTED_JSON = 255
};

class DoubleRatchet {
 public:
  // Longest tolerated gap inside one chain; a frame demanding more skipped
  // keys than this is rejected so a forged seq_num can't force unbounded
  // key derivation.
  static constexpr uint32_t MAX_SKIP = 512;
  // Upper bound on banked out-of-order keys; oldest entries are evicted first.
  static constexpr size_t MAX_SKIPPED_KEYS = 2048;

  DoubleRatchet& init(const Key32& initial_root_key, const DHKeyPair& dh_local,
                      const PubKey& dh_remote, const bool is_initiator);

  std::vector<uint8_t> pq_init(
      const Key32& initial_root_key, const pqdh::KeyPairs& pqdh_local,
      const std::vector<uint8_t>& dh_pub_remote,
      const std::vector<uint8_t>& pq_pub_remote_or_ciphertext,
      const bool is_initiator);

  void dh_ratchet(const DHKeyPair& new_dh_local, const PubKey& new_dh_remote);
  void dh_ratchet_send();
  std::vector<uint8_t> pqdh_ratchet_alice(
      const pqdh::KeyPairs& pqdh_local,
      const std::vector<uint8_t>& dh_pub_remote,
      const std::vector<uint8_t>& pq_pub_remote);
  void pqdh_ratchet_bob(const pqdh::KeyPairs& pqdh_local,
                        const std::vector<uint8_t>& dh_pub_remote,
                        const std::vector<uint8_t>& pq_ciphertext);

  void ratchet_send(Key32& message_key_send);
  void ratchet_receive(Key32& message_key_receive);

  EncryptedMessage encrypt(const std::vector<uint8_t>& plaintext);
  std::vector<uint8_t> decrypt(const EncryptedMessage& msg, bool* ok = nullptr);

  MessageUID get_recv_message_uid() const;
  MessageUID get_send_message_uid() const;

  const std::array<uint8_t, 16>& get_uid() const;
  const uint32_t get_seq_num_send() const;
  const uint32_t get_seq_num_recv() const;
  const uint32_t get_dh_ratchet_step() const;
  size_t skipped_key_count() const { return skipped_keys_.size(); }

  const std::string get_session_key_fingerprint() const;

  const Key32& _debug_root_key() const;
  const Key32& _debug_chain_key_send() const;
  const Key32& _debug_chain_key_receive() const;
  void debug_print() const;

  nlohmann::json to_json() const;
  bool from_json(const nlohmann::json& j);

  ~DoubleRatchet();

  uint8_t error_code{255};

 private:
  Key32 root_key_{};
  DHKeyPair dh_local_{};
  PubKey dh_remote_{};
  Key32 chain_key_send_{};
  Key32 chain_key_receive_{};

  uint32_t seq_num_send_{0};
  uint32_t seq_num_recv_{0};
  uint32_t dh_ratchet_step_{0};

  // Length of our previous sending chain (PN); stamped into every outgoing
  // message so the peer can bank the keys that chain still owes them.
  uint32_t prev_chain_len_{0};
  // dh_ratchet_step value the peer stamps on messages of our current receive
  // chain. The local dh_ratchet_step_ counter mixes send and receive
  // half-ratchets, so it can diverge from the sender's numbering; skipped-key
  // slots must be labeled with the sender's step or late lookups would miss.
  uint32_t recv_chain_label_{0};
  // Message keys for positions the receive chain fast-forwarded over, keyed
  // by (sender step, seq). Each key decrypts exactly one late message and is
  // destroyed on use, so a replayed frame can never decrypt twice.
  std::map<MessageUID, Key32> skipped_keys_;

  std::array<uint8_t, 16> uid_{};

  bool is_initiator_{false};
  // One-shot bootstrap flag, true only for the responder. Our handshake seeds
  // BOTH sides with matching initial keys (unlike raw X3DH, where the
  // initiator's first ratchet key is fresh and unknown to the responder), so
  // neither side's key is ever "new" to the other at the outset — a strict
  // "only ratchet on an unseen remote key" rule would never fire and the DH
  // ratchet would stay frozen at the handshake keys forever. This flag forces
  // exactly one send-side ratchet on the responder's first outgoing message to
  // kick the chain off; it is consumed then and never consulted again. Every
  // later re-key is driven purely by observing a new remote key in decrypt().
  bool pending_initial_send_ratchet_{false};

  static DHKeyPair generate_new_dh_keypair();

  // Atomic DH ratchet step, performed only in reaction to observing a remote
  // public key we have not seen before: derives the receive chain from our
  // CURRENT local key + their new key, then generates a FRESH local key and
  // derives the send chain from it + that same new remote key. Both halves
  // share one root-key chain and one dh_ratchet_step_ increment, since they
  // are one conceptual event, not two independently-triggered ones.
  void advance_to_new_remote_key(const PubKey& new_dh_remote);

  // Advances the receive chain to until_seq, banking every skipped key under
  // chain_label. Returns false (state advanced but caller must discard it)
  // when the gap exceeds MAX_SKIP. Runs on the trial copy inside decrypt().
  bool skip_message_keys(uint32_t until_seq, uint32_t chain_label);
  void prune_skipped_keys();

  std::array<uint8_t, 16> generate_uid(const dr::Key32& root_key,
                                       const dr::PubKey& local_pub,
                                       const dr::PubKey& remote_pub);
};
}  // namespace dr