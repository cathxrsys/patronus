#pragma once

#include <array>
#include <cstdint>

#include "crypto.h"

namespace kdf {

constexpr size_t KEY_LEN = 32;
using Key32 = std::array<uint8_t, KEY_LEN>;

/*
 * Root KDF (Double Ratchet):
 * Input:
 *   previous_root_key - current root key (salt)
 *   dh_secret         - result of DH(local_priv, remote_pub) (32 bytes)
 * Steps:
 *   PRK = HKDF-Extract(salt = previous_root_key, IKM = dh_secret)
 *   OKM = HKDF-Expand(PRK, info="DoubleRatchetRoot", length = 3 * KEY_LEN)
 * Split:
 *   new_root
 *   new_chain_key_send
 *   new_chain_key_receive
 */
void kdf_root(const Key32& previous_root_key, const Key32& dh_secret,
              Key32& new_root, Key32& new_chain_key_send,
              Key32& new_chain_key_receive);

/*
 * Chain KDF (sending):
 *   next_chain_key_send = HMAC(chain_key_send, 0x01)
 *   message_key_send    = HMAC(chain_key_send, 0x02)
 * chain_key_send is updated to next_chain_key_send.
 */
void kdf_chain_send(Key32& chain_key_send, Key32& message_key_send);

/*
 * Chain KDF (receiving):
 *   next_chain_key_receive = HMAC(chain_key_receive, 0x01)
 *   message_key_receive    = HMAC(chain_key_receive, 0x02)
 * chain_key_receive is updated to next_chain_key_receive.
 */
void kdf_chain_receive(Key32& chain_key_receive, Key32& message_key_receive);

}  // namespace kdf