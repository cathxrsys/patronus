package protocol

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
)

const (
	ProtocolVersion  = "3"
	MaxWSMessageSize = 10 * 1024 * 1024
	ChallengeSize    = 64
	MaxPrekeysPerID  = 100
	SyncBatchSize    = 100
	PubKeySize       = 32
	SignatureSize    = 64
	RequestIDSize    = 4
	MessageIDSize    = 8
	DBMessageIDSize  = 8
	MaxFCMTokenSize  = 4096
	// TransferIDSize is the width of the id that ties a chunked transfer's
	// begin/data/end frames together on one connection's outbound stream.
	TransferIDSize = 4
	// ChunkThreshold: frames larger than this are split into ChunkSize pieces so
	// no single frame monopolises the TCP stream. That keeps WS pings/pongs
	// flowing (neither side starves the other) and lets the receiver's liveness
	// watchdog see steady progress instead of mistaking a long transfer for a
	// dead link. Small frames — the overwhelming majority — are sent whole with
	// zero overhead.
	ChunkThreshold = 128 * 1024
	// ChunkSize is the body size of each ChunkData frame. Kept small so that even
	// on a very slow link a chunk lands every few seconds and the connection
	// never looks idle mid-transfer.
	ChunkSize = 32 * 1024
)

const DecoyHTML = `
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Welcome</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            display: flex;
            justify-content: center;
            align-items: center;
            height: 100vh;
            margin: 0;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
        }
        .container {
            text-align: center;
            color: white;
        }
        h1 {
            font-size: 3em;
            margin: 0;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>Hello, World!</h1>
    </div>
</body>
</html>
`

type RequestType byte

const (
	RequestSendMessage      RequestType = 0
	RequestGetPrekey        RequestType = 1
	RequestSendPrekey       RequestType = 2
	RequestCountPrekeys     RequestType = 3
	RequestOnlineStatus     RequestType = 4
	RequestAudioFrame       RequestType = 5
	RequestReadedSignal     RequestType = 6
	RequestReceivedSignal   RequestType = 7
	RequestRegisterFCMToken RequestType = 8
	RequestSessionRemove    RequestType = 9
	RequestMessageAck       RequestType = 10
	// RequestRevokeIdentity submits a signed "this key is compromised"
	// certificate. Body: the 64-byte ed25519 signature over
	// RevocationSigningMessage(identity). The identity being revoked is the
	// authenticated session identity, so a client can only ever revoke its own
	// key. The revocation is terminal and monotonic.
	RequestRevokeIdentity RequestType = 11
	// RequestEnrollMember admits a new identity to the server whitelist. Only an
	// existing member may send it; the body is the 32-byte public key of the
	// contact that scanned the sender's QR and established a session. This is how
	// membership propagates through the invite graph without editing the config.
	// Body: [32-byte target identity].
	RequestEnrollMember RequestType = 12
)

// RevocationContext is the domain-separation prefix that is signed together with
// the identity public key to produce a key-revocation certificate. Signing with
// the identity key proves the owner authorised the revocation. Because the
// revocation is terminal and irreversible, the fact that an attacker holding the
// key could also produce it does not matter — that is exactly the desired
// outcome. Both client and server MUST build this message identically.
const RevocationContext = "PATRONUS_KEY_REVOCATION_v1"

// RevocationSigningMessage returns the exact bytes that a key-revocation
// certificate signs: the domain-separation context followed by the raw identity
// public key.
func RevocationSigningMessage(identity []byte) []byte {
	msg := make([]byte, 0, len(RevocationContext)+len(identity))
	msg = append(msg, RevocationContext...)
	msg = append(msg, identity...)
	return msg
}

// PushKind is a non-sensitive, server-readable hint carried as the first byte of
// a RequestSendMessage body. It tells the server which kind of FCM push to emit
// when the recipient is offline, without exposing the (still E2E-encrypted)
// payload. The payload itself is unaffected and never inspected.
type PushKind byte

const (
	// PushKindDefault is a regular message: notify the recipient with the
	// standard "new message" push when they are offline.
	PushKindDefault PushKind = 0
	// PushKindCall is a call_request: wake the recipient with a data-only call
	// push (no notification payload, no sensitive data — just the fact of a
	// call) so the app can come online, receive the call_request and ring.
	PushKindCall PushKind = 1
	// PushKindSilent is call signaling other than the initial request
	// (accept/discard) or any control message that must be stored for later
	// delivery without ever raising a push.
	PushKindSilent PushKind = 2
	// PushKindCallCancel is the caller cancelling the call. When the recipient is
	// offline it wakes them with a data-only call_cancel push so a ringing
	// notification (raised by an earlier PushKindCall) stops and is dismissed.
	PushKindCallCancel PushKind = 3
)

type ResponseType byte

const (
	ResponseReceived         ResponseType = 0
	ResponseDelivered        ResponseType = 1
	ResponseForward          ResponseType = 2
	ResponsePrekey           ResponseType = 3
	ResponsePrekeysCount     ResponseType = 4
	ResponseAuthSuccess      ResponseType = 5
	ResponseOnlineStatus     ResponseType = 6
	ResponseAudioFrame       ResponseType = 7
	ResponseSyncEnd          ResponseType = 8
	ResponseSyncStart        ResponseType = 9
	ResponseReadedSignal     ResponseType = 10
	ResponseReceivedSignal   ResponseType = 11
	ResponseProtocolMismatch ResponseType = 12
	ResponseFCMTokenReceived ResponseType = 13
	ResponseFCMNotUsed       ResponseType = 14
	ResponseSessionRemoved   ResponseType = 15
	ResponseForwardLive      ResponseType = 16
	// ResponseIdentityRevoked tells a client that the identity it is trying to
	// reach has published a key-revocation certificate: the key is compromised
	// and must no longer be trusted. Frame: [type][32-byte revoked identity].
	ResponseIdentityRevoked ResponseType = 17
	// ResponseEnrollAck confirms a RequestEnrollMember was applied (the target
	// is now whitelisted, or already was). The enroller uses this to know it
	// can stop retrying that enrollment on future reconnects.
	ResponseEnrollAck ResponseType = 18
	// ResponseAuthenticated is sent the instant the challenge signature verifies,
	// before offline sync begins. It lets the client show a definite
	// "authenticated — now synchronizing" state instead of a stuck
	// "authenticating" while a slow backlog drains. The client still withholds
	// its own outbound storm until ResponseAuthSuccess (sent after sync).
	ResponseAuthenticated ResponseType = 19
	// ResponseChunkBegin/Data/End carry a frame too large to send in one piece
	// (see ChunkThreshold). Begin announces [transferID, totalLen], Data appends
	// bytes, End signals completion — at which point the receiver reassembles the
	// original frame and dispatches it as if it had arrived whole. This is a
	// transport-level concern, entirely below the E2E layer (the server just
	// splits an already-encrypted blob; the client reassembles then decrypts).
	ResponseChunkBegin ResponseType = 20
	ResponseChunkData  ResponseType = 21
	ResponseChunkEnd   ResponseType = 22
)

var ErrFrameTooShort = errors.New("frame too short")

func ParseRequestEnvelope(frame []byte) (RequestType, uint32, []byte, error) {
	if len(frame) < 1+RequestIDSize {
		return 0, 0, nil, ErrFrameTooShort
	}

	requestType := RequestType(frame[0])
	requestID := binary.BigEndian.Uint32(frame[1 : 1+RequestIDSize])
	body := frame[1+RequestIDSize:]
	return requestType, requestID, body, nil
}

func PutUint32(dst []byte, value uint32) {
	binary.BigEndian.PutUint32(dst, value)
}

func PutUint64(dst []byte, value uint64) {
	binary.BigEndian.PutUint64(dst, value)
}

func ReceivedAck(requestID uint32) []byte {
	msg := make([]byte, 1+RequestIDSize)
	msg[0] = byte(ResponseReceived)
	PutUint32(msg[1:], requestID)
	return msg
}

func FCMTokenReceivedAck(requestID uint32) []byte {
	msg := make([]byte, 1+RequestIDSize)
	msg[0] = byte(ResponseFCMTokenReceived)
	PutUint32(msg[1:], requestID)
	return msg
}

func FCMNotUsedAck(requestID uint32) []byte {
	msg := make([]byte, 1+RequestIDSize)
	msg[0] = byte(ResponseFCMNotUsed)
	PutUint32(msg[1:], requestID)
	return msg
}

func EnrollAck(requestID uint32) []byte {
	msg := make([]byte, 1+RequestIDSize)
	msg[0] = byte(ResponseEnrollAck)
	PutUint32(msg[1:], requestID)
	return msg
}

func OnlineStatus(requestID uint32, isOnline bool, originalPayload []byte) []byte {
	msg := make([]byte, 1+RequestIDSize+1+len(originalPayload))
	msg[0] = byte(ResponseOnlineStatus)
	PutUint32(msg[1:], requestID)
	if isOnline {
		msg[5] = 1
	}
	copy(msg[6:], originalPayload)
	return msg
}

func Forward(from, payload []byte) []byte {
	msg := make([]byte, 1+len(from)+len(payload))
	msg[0] = byte(ResponseForward)
	copy(msg[1:], from)
	copy(msg[1+len(from):], payload)
	return msg
}

func ForwardLive(from, payload []byte, dbID int64) []byte {
	msg := make([]byte, 1+len(from)+DBMessageIDSize+len(payload))
	msg[0] = byte(ResponseForwardLive)
	copy(msg[1:], from)
	PutUint64(msg[1+len(from):], uint64(dbID))
	copy(msg[1+len(from)+DBMessageIDSize:], payload)
	return msg
}

func AudioFrame(from, payload []byte) []byte {
	msg := make([]byte, 1+len(from)+len(payload))
	msg[0] = byte(ResponseAudioFrame)
	copy(msg[1:], from)
	copy(msg[1+len(from):], payload)
	return msg
}

func ReadedSignal(from []byte) []byte {
	msg := make([]byte, 1+len(from))
	msg[0] = byte(ResponseReadedSignal)
	copy(msg[1:], from)
	return msg
}

func ReceivedSignal(from, messageID []byte) []byte {
	msg := make([]byte, 1+len(from)+len(messageID))
	msg[0] = byte(ResponseReceivedSignal)
	copy(msg[1:], from)
	copy(msg[1+len(from):], messageID)
	return msg
}

func SessionRemovedSignal(from []byte) []byte {
	msg := make([]byte, 1+len(from))
	msg[0] = byte(ResponseSessionRemoved)
	copy(msg[1:], from)
	return msg
}

func IdentityRevoked(identity []byte) []byte {
	msg := make([]byte, 1+len(identity))
	msg[0] = byte(ResponseIdentityRevoked)
	copy(msg[1:], identity)
	return msg
}

func SyncMarker(responseType ResponseType) []byte {
	return []byte{byte(responseType)}
}

func PrekeysCount(requestID uint32, count byte) []byte {
	msg := make([]byte, 1+RequestIDSize+1)
	msg[0] = byte(ResponsePrekeysCount)
	PutUint32(msg[1:], requestID)
	msg[5] = count
	return msg
}

func PrekeyResponse(requestID uint32, payload []byte) []byte {
	msg := make([]byte, 1+RequestIDSize+len(payload))
	msg[0] = byte(ResponsePrekey)
	PutUint32(msg[1:], requestID)
	copy(msg[5:], payload)
	return msg
}

func AuthSuccess(accessToken string) []byte {
	msg := make([]byte, 1+len(accessToken))
	msg[0] = byte(ResponseAuthSuccess)
	copy(msg[1:], accessToken)
	return msg
}

// Authenticated is a bare marker sent right after the signature verifies and
// before sync — see ResponseAuthenticated.
func Authenticated() []byte {
	return []byte{byte(ResponseAuthenticated)}
}

// ChunkBegin/ChunkData/ChunkEnd frame a single large payload as a sequence of
// small frames tied together by transferID (see ChunkThreshold / ChunkSize).
func ChunkBegin(transferID, totalLen uint32) []byte {
	msg := make([]byte, 1+TransferIDSize+4)
	msg[0] = byte(ResponseChunkBegin)
	PutUint32(msg[1:], transferID)
	PutUint32(msg[1+TransferIDSize:], totalLen)
	return msg
}

func ChunkData(transferID uint32, data []byte) []byte {
	msg := make([]byte, 1+TransferIDSize+len(data))
	msg[0] = byte(ResponseChunkData)
	PutUint32(msg[1:], transferID)
	copy(msg[1+TransferIDSize:], data)
	return msg
}

func ChunkEnd(transferID uint32) []byte {
	msg := make([]byte, 1+TransferIDSize)
	msg[0] = byte(ResponseChunkEnd)
	PutUint32(msg[1:], transferID)
	return msg
}

func ProtocolMismatch(payload []byte) []byte {
	msg := make([]byte, 1+len(payload))
	msg[0] = byte(ResponseProtocolMismatch)
	copy(msg[1:], payload)
	return msg
}

func HexIdentity(raw []byte) string {
	return hex.EncodeToString(raw)
}

func RecomputePrekeyID(dhPub, pqPub, edPub []byte) []byte {
	hasher := sha256.New()
	hasher.Write(dhPub)
	hasher.Write(pqPub)
	hasher.Write(edPub)
	return hasher.Sum(nil)
}
