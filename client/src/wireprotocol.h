#ifndef WIREPROTOCOL_H
#define WIREPROTOCOL_H

// Binary wire-format layout constants shared by the client frame parsers.
//
// These mirror the server definitions in
// server/internal/protocol/protocol.go (PubKeySize, RequestIDSize,
// MessageIDSize, ChallengeSize, ...). Both sides MUST agree: never change an
// offset here without the matching change on the server.
//
// The field sizes are the primitives; the derived *Offset / *Min / *Header
// values are computed from them, so a format change touches one line instead of
// a scatter of magic numbers, and the very same constants drive both the read
// offsets and the minimum-length bounds checks.
namespace wire {

// --- Field sizes (bytes) ---
inline constexpr int kTypeSize = 1;          // leading response/request type byte
inline constexpr int kRequestIdSize = 4;     // RequestIDSize
inline constexpr int kPubKeySize = 32;       // PubKeySize (sender identity key)
inline constexpr int kMessageIdSize = 8;     // MessageIDSize / DBMessageIDSize
inline constexpr int kOnlineFlagSize = 1;    // online/offline flag byte
inline constexpr int kMsgFormatSize = 1;     // EncryptedMessageFormat byte
inline constexpr int kPrekeysCountSize = 1;  // prekey count byte
inline constexpr int kChallengeSize = 64;    // ChallengeSize (auth challenge)

// --- Derived offsets and minimum frame lengths ---
// Offset of the sender identity key (right after the type byte).
inline constexpr int kFromKeyOffset = kTypeSize;  // 1
// Offset of the body that follows [type][requestId].
inline constexpr int kBodyAfterRequestId = kTypeSize + kRequestIdSize;  // 5
// Offset of the contact key in an ONLINE_STATUS frame:
// [type][requestId][onlineFlag][key...].
inline constexpr int kOnlineKeyOffset = kBodyAfterRequestId + kOnlineFlagSize;  // 6
// Offset of the byte that follows [type][fromKey] in a full frame (message id /
// db id / format byte / payload, depending on the frame).
inline constexpr int kPayloadAfterFromKey = kTypeSize + kPubKeySize;  // 33
// Minimum FORWARD frame: [type][fromKey][format].
inline constexpr int kForwardMinSize = kPayloadAfterFromKey + kMsgFormatSize;  // 34
// Minimum RECEIVED_SIGNAL frame: [type][fromKey][messageId].
inline constexpr int kReceivedSignalMin = kPayloadAfterFromKey + kMessageIdSize;  // 41
// FORWARD_LIVE header: [type][fromKey][dbId] before the wrapped payload.
inline constexpr int kForwardLiveHeader = kPayloadAfterFromKey + kMessageIdSize;  // 41
// Minimum PREKEYS_COUNT frame: [type][requestId][count].
inline constexpr int kPrekeysCountMin = kBodyAfterRequestId + kPrekeysCountSize;  // 6

}  // namespace wire

#endif  // WIREPROTOCOL_H
