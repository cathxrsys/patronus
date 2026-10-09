package server

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"

	"patronus/server/internal/protocol"
	"patronus/server/internal/storage"
)

type outboundItem struct {
	data []byte
	done chan error
}

type prekeyPayload struct {
	ID         string `json:"id"`
	DHPub      string `json:"dh_pub"`
	PQPub      string `json:"pq_pub"`
	Ed25519Pub string `json:"ed25519_pub"`
	Signature  string `json:"signature"`
}

type fcmTokenPayload struct {
	Token string `json:"token"`
}

type Session struct {
	server                *Server
	conn                  *websocket.Conn
	remoteAddr            string
	clientProtocolVersion string
	logger                *slog.Logger
	identity              []byte
	identityHex           string
	accessToken           string
	// provisional is set for an authenticated identity that is not (yet) on the
	// whitelist. Such a session may establish a session with a member so the
	// member can enroll it, but it cannot enroll others, register for push, or
	// deposit messages to non-members. It is atomic because a member enrolling
	// this identity (from another session's goroutine) promotes it live, without
	// requiring a reconnect (see handleEnrollMember).
	provisional atomic.Bool
	// provisionalDeposits counts messages deposited while provisional, to enforce
	// the deposit budget that caps flooding by a not-yet-enrolled client.
	provisionalDeposits atomic.Int64
	// pendingFCMToken holds a token the client tried to register while still
	// provisional, so it can be applied the moment handleEnrollMember promotes
	// this session — the client typically registers immediately after auth,
	// which routinely races the enroller's message and would otherwise strand
	// the identity without push until its next reconnect.
	pendingFCMToken atomic.Pointer[string]

	// transferCounter mints a per-connection id for each chunked outbound
	// transfer so the client can reassemble even when two transfers' chunks
	// interleave on this connection's stream (see sendChunked).
	transferCounter atomic.Uint32

	mu        sync.Mutex
	cond      *sync.Cond
	highQueue []outboundItem
	lowQueue  []outboundItem
	closed    bool
	closeOnce sync.Once
	done      chan struct{}
}

func NewSession(server *Server, conn *websocket.Conn, remoteAddr string, clientProtocolVersion string) *Session {
	session := &Session{
		server:                server,
		conn:                  conn,
		remoteAddr:            remoteAddr,
		clientProtocolVersion: clientProtocolVersion,
		logger:                server.logger.With(slog.String("remote_addr", remoteAddr)),
		done:                  make(chan struct{}),
	}
	session.cond = sync.NewCond(&session.mu)
	return session
}

func (s *Session) Run(ctx context.Context) {
	go s.writeLoop()
	defer s.shutdown(nil)

	if ok := s.validateProtocolVersion(); !ok {
		return
	}

	if err := s.authenticate(); err != nil {
		s.logger.Warn("authentication failed", slog.Any("error", err))
		return
	}

	// Membership gate: a valid signature only proves key ownership. Whitelisted
	// identities are full members; anyone else runs a provisional session (see
	// the field doc) so a brand-new person can bootstrap in by scanning a
	// member's QR and being enrolled by that member. The gate is enforced per
	// operation below, not by dropping the connection.
	whitelisted, err := s.server.isWhitelisted(ctx, s.identityHex)
	if err != nil {
		s.logger.Warn("whitelist check failed", slog.Any("error", err), slog.String("identity", s.identityHex))
		return
	}
	s.provisional.Store(!whitelisted)
	if !whitelisted {
		s.logger.Info("provisional (non-member) session", slog.String("identity", s.identityHex))
	}

	// Keepalive has to be live BEFORE we start draining the backlog. The offline
	// sync below can take a while (large backlog, slow mobile link), and the
	// client runs a dead-connection watchdog that drops the socket if it does not
	// get a WS pong in time. gorilla only answers an incoming ping from inside
	// ReadMessage, so we must already be in the read loop while syncing —
	// otherwise a slow sync never finishes: the client tears the connection down
	// mid-sync and reconnects forever without ever receiving SYNC_END.
	const (
		pingInterval = 30 * time.Second
		pongTimeout  = 90 * time.Second
	)
	if err := s.conn.SetReadDeadline(time.Now().Add(pongTimeout)); err != nil {
		s.logger.Warn("failed to set read deadline", slog.Any("error", err))
		return
	}
	s.conn.SetPongHandler(func(string) error {
		return s.conn.SetReadDeadline(time.Now().Add(pongTimeout))
	})
	go func() {
		ticker := time.NewTicker(pingInterval)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				if err := s.conn.WriteControl(websocket.PingMessage, nil, time.Now().Add(10*time.Second)); err != nil {
					return
				}
			case <-s.done:
				return
			}
		}
	}()

	// Run the read loop in the background so the connection keeps answering the
	// client's pings while we drain the (possibly large) offline backlog below.
	// gorilla only replies to an incoming ping from inside ReadMessage, so
	// without a concurrent reader a slow sync would stall until the client's
	// dead-connection watchdog tears the socket down and reconnects — forever.
	go s.readLoop(ctx)

	// Tell the client it is authenticated the instant the handshake is done, so
	// it can show "synchronizing" instead of a stuck "authenticating" while the
	// backlog below drains. This is only a UI/liveness signal — the client still
	// holds its own outbound (prekey top-up, retries) until AUTH_SUCCESS after
	// sync, so nothing interleaves with the sync stream.
	if err := s.send(protocol.Authenticated(), false); err != nil {
		s.logger.Warn("failed to send authenticated marker", slog.Any("error", err), slog.String("identity", s.identityHex))
		return
	}

	// Drain everything buffered while this identity was offline, bracketed by
	// SYNC_START/SYNC_END, then register for live delivery and release the client
	// with AUTH_SUCCESS. Ordering matters: the client deliberately holds back its
	// own outbound work (prekey top-up, message retries) until AUTH_SUCCESS, and
	// we register for live delivery only after the backlog is flushed, so nothing
	// interleaves with the sync stream. Keeping this sequential in the main
	// goroutine also keeps sessions.Add strictly ordered before the shutdown that
	// removes it, so a mid-sync disconnect can never leave a dead session mapped.
	if err := s.syncOfflineMessages(ctx); err != nil {
		s.logger.Warn("offline sync failed", slog.Any("error", err), slog.String("identity", s.identityHex))
		return
	}

	s.server.sessions.Add(s.identityHex, s)
	s.logger.Info("sending auth success",
		slog.String("identity", s.identityHex),
		slog.Int("token_length", len(s.accessToken)),
		slog.Bool("token_empty", s.accessToken == ""))
	if err := s.send(protocol.AuthSuccess(s.accessToken), false); err != nil {
		s.logger.Warn("failed to send auth success", slog.Any("error", err), slog.String("identity", s.identityHex))
		return
	}

	// Session is up; park here until the client disconnects (the read loop ends
	// and triggers shutdown, closing done) or the session is otherwise torn down.
	<-s.done
}

func (s *Session) readLoop(ctx context.Context) {
	// A read error means the client is gone; tear the whole session down so the
	// writer, the keepalive goroutine and Run (parked on s.done) all unwind.
	defer s.shutdown(nil)

	for {
		_, frame, err := s.conn.ReadMessage()
		if err != nil {
			if !websocket.IsCloseError(err, websocket.CloseNormalClosure, websocket.CloseGoingAway) {
				s.logger.Debug("read loop stopped", slog.Any("error", err), slog.String("identity", s.identityHex))
			}
			return
		}

		if len(frame) < 1+protocol.RequestIDSize {
			continue
		}

		requestType := protocol.RequestType(frame[0])
		payload := frame[1:]

		switch requestType {
		case protocol.RequestAudioFrame:
			s.handleAudioFrame(ctx, payload)
			continue
		case protocol.RequestReadedSignal:
			s.handleReadedSignal(ctx, payload)
			continue
		case protocol.RequestReceivedSignal:
			s.handleReceivedSignal(ctx, payload)
			continue
		case protocol.RequestSessionRemove:
			s.handleSessionRemoveSignal(ctx, payload)
			continue
		case protocol.RequestMessageAck:
			s.handleMessageAck(ctx, payload)
			continue
		}

		_, requestID, body, err := protocol.ParseRequestEnvelope(frame)
		if err != nil {
			continue
		}

		s.handleRequest(ctx, requestType, requestID, body)
	}
}

func (s *Session) validateProtocolVersion() bool {
	if s.clientProtocolVersion == protocol.ProtocolVersion {
		return true
	}

	reason := "protocol_version_mismatch"
	message := "client and server protocol versions are incompatible"
	if s.clientProtocolVersion == "" {
		reason = "missing_protocol_version"
		message = "client did not send X-Protocol-Version"
	}

	payload := mustJSON(map[string]any{
		"reason":                  reason,
		"message":                 message,
		"server_protocol_version": protocol.ProtocolVersion,
		"client_protocol_version": s.clientProtocolVersion,
	})

	s.logger.Warn("protocol version rejected",
		slog.String("server_protocol_version", protocol.ProtocolVersion),
		slog.String("client_protocol_version", s.clientProtocolVersion),
		slog.String("reason", reason))

	if err := s.send(protocol.ProtocolMismatch(payload), true); err != nil {
		s.logger.Warn("failed to send protocol mismatch response", slog.Any("error", err))
	}

	return false
}

func (s *Session) authenticate() error {
	challenge := make([]byte, protocol.ChallengeSize)
	if _, err := rand.Read(challenge); err != nil {
		return fmt.Errorf("generate challenge: %w", err)
	}
	defer zeroBytes(challenge)

	if err := s.send(challenge, false); err != nil {
		return fmt.Errorf("send challenge: %w", err)
	}

	_, authData, err := s.conn.ReadMessage()
	if err != nil {
		return fmt.Errorf("read auth payload: %w", err)
	}
	if len(authData) != protocol.PubKeySize+protocol.SignatureSize {
		return errors.New("invalid auth payload size")
	}

	pubKey := append([]byte(nil), authData[:protocol.PubKeySize]...)
	signature := authData[protocol.PubKeySize:]
	if !ed25519.Verify(ed25519.PublicKey(pubKey), challenge, signature) {
		zeroBytes(pubKey)
		return errors.New("signature verification failed")
	}

	s.identity = pubKey
	s.identityHex = hex.EncodeToString(pubKey)

	accessToken, err := generateAccessToken()
	if err != nil {
		zeroBytes(pubKey)
		return fmt.Errorf("generate access token: %w", err)
	}
	s.accessToken = accessToken
	s.logger.Info("access token generated",
		slog.String("identity", s.identityHex),
		slog.Int("token_length", len(s.accessToken)),
		slog.Bool("token_empty", s.accessToken == ""))

	s.logger = s.logger.With(slog.String("identity", s.identityHex))
	s.logger.Info("client authenticated")
	return nil
}

func (s *Session) syncOfflineMessages(ctx context.Context) error {
	if err := s.send(protocol.SyncMarker(protocol.ResponseSyncStart), false); err != nil {
		return err
	}

	// Messages: stream the backlog oldest-first via an id cursor and do NOT
	// delete here. Each message goes out as a live-style frame carrying its db
	// id; the client ACKs on receipt and that ACK — handled concurrently in
	// readLoop — is what deletes it (see handleMessageAck). Making deletion
	// contingent on a real client ACK means an interrupted sync can never drop a
	// message that was sent but not yet confirmed: it stays in the backlog and
	// re-delivers next time, and the client dedupes by message id. A big/slow or
	// undecryptable message therefore can't wedge sync — whatever the client
	// actually received is removed, so every reconnect makes forward progress
	// instead of restarting from the head. The cursor (id strictly increasing)
	// keeps the loop terminating even though nothing is deleted inside it.
	var afterID int64
	for {
		rows, err := s.server.repo.FetchMessagesAfter(ctx, s.identityHex, afterID, protocol.SyncBatchSize)
		if err != nil {
			return err
		}
		if len(rows) == 0 {
			break
		}

		for _, row := range rows {
			fromBytes, err := hex.DecodeString(row.FromAddr)
			if err != nil {
				return fmt.Errorf("decode sender identity: %w", err)
			}
			if err := s.send(protocol.ForwardLive(fromBytes, row.Payload, row.ID), false); err != nil {
				return err
			}
			afterID = row.ID
		}
	}

	for {
		rows, err := s.server.repo.FetchSignals(ctx, s.identityHex, protocol.SyncBatchSize)
		if err != nil {
			return err
		}
		if len(rows) == 0 {
			break
		}

		for _, row := range rows {
			fromBytes, err := hex.DecodeString(row.FromAddr)
			if err != nil {
				return fmt.Errorf("decode signal sender identity: %w", err)
			}

			response, err := buildStoredSignalResponse(row.SignalType, fromBytes, row.MessageID)
			if err != nil {
				return fmt.Errorf("build stored signal response: %w", err)
			}

			if err := s.send(response, false); err != nil {
				return err
			}

			if err := s.server.repo.DeleteSignal(ctx, row.SignalType, row.FromAddr, row.ToAddr, row.MessageID); err != nil {
				return err
			}
		}
	}

	return s.send(protocol.SyncMarker(protocol.ResponseSyncEnd), false)
}

func (s *Session) handleRequest(ctx context.Context, requestType protocol.RequestType, requestID uint32, body []byte) {
	switch requestType {
	case protocol.RequestOnlineStatus:
		s.handleOnlineStatus(requestID, body)
	case protocol.RequestSendMessage:
		s.handleSendMessage(ctx, requestID, body)
	case protocol.RequestCountPrekeys:
		s.handleCountPrekeys(ctx, requestID)
	case protocol.RequestSendPrekey:
		s.handleSendPrekeys(ctx, body)
	case protocol.RequestGetPrekey:
		s.handleGetPrekey(ctx, requestID, body)
	case protocol.RequestRegisterFCMToken:
		// Push is a member-only benefit; a provisional identity gets no FCM. Ack
		// it as "not used" so the client stops waiting instead of hanging, but
		// stash the token first: enrollment routinely completes a few hundred
		// milliseconds later on another session's goroutine (see
		// handleEnrollMember), and without this the token would otherwise be
		// lost until the client happens to reconnect.
		if s.provisional.Load() {
			s.logger.Debug("ignoring fcm registration from provisional session, stashing for possible promotion", slog.String("identity", s.identityHex))
			if token, err := parseFCMTokenPayload(body); err == nil {
				s.pendingFCMToken.Store(&token)
			}
			if err := s.send(protocol.FCMNotUsedAck(requestID), false); err != nil {
				s.logger.Warn("failed to ack provisional fcm registration", slog.Any("error", err))
			}
			return
		}
		s.handleRegisterFCMToken(ctx, requestID, body)
	case protocol.RequestRevokeIdentity:
		s.handleRevokeIdentity(ctx, body)
	case protocol.RequestEnrollMember:
		s.handleEnrollMember(ctx, requestID, body)
	}
}

// handleEnrollMember admits a new identity to the server's whitelist. Only an
// existing member may enroll; the target is the 32-byte public key of the
// contact that just scanned the member's QR and established a session. This is
// how membership propagates through the invite graph without the operator
// editing the config. The optional flat member cap is enforced here.
//
// The enroller's client persists this as a pending operation and retries it on
// every reconnect until acknowledged (see ResponseEnrollAck), since a dropped
// connection at the wrong moment would otherwise silently and permanently
// leave the target provisional. Every success path below - including the
// idempotent "already a member" case - must ack, or the client would retry
// forever; every path that leaves the target NOT a member must NOT ack, so the
// client keeps retrying.
func (s *Session) handleEnrollMember(ctx context.Context, requestID uint32, body []byte) {
	if !s.server.gateEnabled {
		// Nothing to maintain when the gate is disabled - everyone is already a
		// full member, so this is trivially satisfied.
		s.ackEnroll(requestID)
		return
	}
	if s.provisional.Load() {
		s.logger.Warn("rejecting enroll from provisional session", slog.String("identity", s.identityHex))
		return
	}
	if len(body) < protocol.PubKeySize {
		return
	}

	targetHex := protocol.HexIdentity(body[:protocol.PubKeySize])

	// Idempotent no-op if already a member — avoids a needless cap check and log.
	if already, err := s.server.repo.IsWhitelisted(ctx, targetHex); err != nil {
		s.logger.Warn("enroll whitelist lookup failed", slog.Any("error", err), slog.String("target", targetHex))
		return
	} else if already {
		s.ackEnroll(requestID)
		return
	}

	if memberCap := s.server.cfg.TreeMaxMembers; memberCap >= 0 {
		count, err := s.server.repo.CountWhitelist(ctx)
		if err != nil {
			s.logger.Warn("enroll member cap check failed", slog.Any("error", err))
			return
		}
		if count >= memberCap {
			s.logger.Warn("member cap reached, refusing enroll",
				slog.Int("cap", memberCap), slog.String("target", targetHex), slog.String("enroller", s.identityHex))
			return
		}
	}

	if err := s.server.repo.AddToWhitelist(ctx, targetHex); err != nil {
		s.logger.Error("failed to enroll member", slog.Any("error", err), slog.String("target", targetHex))
		return
	}
	s.logger.Info("member enrolled", slog.String("target", targetHex), slog.String("enroller", s.identityHex))
	s.ackEnroll(requestID)

	// Promote the newly-admitted identity's live session (if any) immediately, so
	// it gains full member rights — most importantly the ability to enroll its own
	// contacts — without waiting for a reconnect. Without this, transitive growth
	// would stall for one session (a just-admitted member could not bring others).
	if target := s.server.sessions.Find(targetHex); target != nil {
		target.provisional.Store(false)
		s.logger.Info("promoted live session to member", slog.String("identity", targetHex))
		target.flushPendingFCMToken(ctx)
	}
}

func (s *Session) ackEnroll(requestID uint32) {
	if err := s.send(protocol.EnrollAck(requestID), false); err != nil {
		s.logger.Warn("failed to ack enroll member", slog.Any("error", err))
	}
}

// flushPendingFCMToken applies a token stashed by handleRequest while this
// session was still provisional (see pendingFCMToken). Called right after
// promotion so a client that registered its token immediately after auth —
// before the enroller's message reached the server — doesn't end up stranded
// without push until it happens to reconnect.
func (s *Session) flushPendingFCMToken(ctx context.Context) {
	tokenPtr := s.pendingFCMToken.Swap(nil)
	if tokenPtr == nil {
		return
	}
	if !s.server.cfg.FCMEnabled() || s.server.notifier == nil {
		return
	}

	token := *tokenPtr
	fingerprint := fcmTokenFingerprint(token)
	if err := s.server.repo.UpsertFCMToken(ctx, s.identityHex, token); err != nil {
		s.logger.Error("failed to register stashed fcm token after promotion", slog.Any("error", err))
		return
	}
	s.logger.Info("registered stashed fcm token after promotion",
		slog.String("identity", s.identityHex), slog.String("token_fingerprint", fingerprint))
}

// handleRevokeIdentity records a signed "my key is compromised" certificate for
// the authenticated identity. Because the target is always the session identity,
// a client can only revoke its own key. The certificate must be a valid ed25519
// signature by that identity over protocol.RevocationSigningMessage; this is
// verified even though the session is already authenticated, so a stored
// tombstone is always independently checkable. Revocation is terminal: once
// recorded the key can never be trusted again, its outstanding prekeys are
// dropped, and getprekey/sendMessage against it are refused (see handleGetPrekey
// and handleSendMessage).
func (s *Session) handleRevokeIdentity(ctx context.Context, body []byte) {
	if len(body) != protocol.SignatureSize {
		s.logger.Warn("rejected revocation with bad signature length", slog.Int("len", len(body)))
		return
	}

	signingMessage := protocol.RevocationSigningMessage(s.identity)
	if !ed25519.Verify(ed25519.PublicKey(s.identity), signingMessage, body) {
		s.logger.Warn("rejected revocation with invalid signature")
		return
	}

	if err := s.server.repo.StoreRevocation(ctx, s.identityHex, body); err != nil {
		s.logger.Error("failed to store revocation", slog.Any("error", err))
		return
	}

	// Drop any outstanding prekeys immediately so no new session can be
	// bootstrapped against the revoked key even before a getprekey is attempted.
	if err := s.server.repo.DeletePrekeysForIdentity(ctx, s.identity); err != nil {
		s.logger.Warn("failed to delete prekeys for revoked identity", slog.Any("error", err))
	}

	s.logger.Warn("identity key revoked", slog.String("identity", s.identityHex))
}

func (s *Session) handleOnlineStatus(requestID uint32, body []byte) {
	targetIdentity := protocol.HexIdentity(body)
	isOnline := s.server.sessions.Find(targetIdentity) != nil

	if err := s.send(protocol.OnlineStatus(requestID, isOnline, body), false); err != nil {
		s.logger.Warn("failed to send online status", slog.Any("error", err))
	}
}

// chargeProvisionalDeposit accounts one storage-producing action (a stored
// message or received signal) against the provisional deposit budget. It returns
// false once a not-yet-enrolled session exceeds the budget, so the caller drops
// the action. Non-provisional / promoted sessions, and a negative budget, are
// always allowed.
func (s *Session) chargeProvisionalDeposit() bool {
	if !s.provisional.Load() {
		return true
	}
	budget := s.server.provisionalDepositBudget
	if budget < 0 {
		return true
	}
	return s.provisionalDeposits.Add(1) <= int64(budget)
}

// allowProvisionalTarget is the gate for every discrete (one-shot-per-event)
// request type a provisional (not-yet-enrolled) session may send toward
// another identity: messages, read/received/session-removed signals. A
// provisional sender may only ever reach an actual member — enough to
// establish a session with whoever will enroll them — never another
// non-member, or the server becomes an open relay between two strangers.
// Charges the shared provisional deposit budget as a side effect. Always
// true for a member session (chargeProvisionalDeposit itself is then a
// no-op).
func (s *Session) allowProvisionalTarget(ctx context.Context, targetHex string) bool {
	if !s.provisional.Load() {
		return true
	}

	if !s.chargeProvisionalDeposit() {
		s.logger.Warn("provisional deposit budget exceeded, dropping",
			slog.String("from", s.identityHex), slog.String("to", targetHex))
		return false
	}

	targetWhitelisted, err := s.server.repo.IsWhitelisted(ctx, targetHex)
	if err != nil {
		s.logger.Warn("provisional target whitelist check failed", slog.Any("error", err), slog.String("to", targetHex))
		return false
	}
	if !targetWhitelisted {
		s.logger.Warn("dropping provisional traffic to non-member", slog.String("from", s.identityHex), slog.String("to", targetHex))
		return false
	}
	return true
}

// allowProvisionalStream is the audio-frame equivalent of
// allowProvisionalTarget. It deliberately does NOT charge the one-shot
// deposit budget: a live call relays dozens of audio frames per second, and
// charging per frame would exhaust a budget sized for "a couple of handshake
// messages" a few seconds into any provisional-sender call, cutting the
// audio out from under a legitimate in-progress call. It still confirms the
// target is an actual member, so two non-members cannot hold a full call by
// exchanging raw audio frames with no membership check at all.
func (s *Session) allowProvisionalStream(ctx context.Context, targetHex string) bool {
	if !s.provisional.Load() {
		return true
	}

	targetWhitelisted, err := s.server.repo.IsWhitelisted(ctx, targetHex)
	if err != nil {
		s.logger.Warn("provisional audio target whitelist check failed", slog.Any("error", err), slog.String("to", targetHex))
		return false
	}
	if !targetWhitelisted {
		s.logger.Warn("dropping provisional audio frame to non-member", slog.String("from", s.identityHex), slog.String("to", targetHex))
		return false
	}
	return true
}

func (s *Session) handleSendMessage(ctx context.Context, requestID uint32, body []byte) {
	// Body layout: [1-byte push kind][32-byte target identity][encrypted payload].
	// The push kind is a non-sensitive routing hint (see protocol.PushKind); the
	// payload remains end-to-end encrypted and is never inspected here.
	if len(body) < 1+protocol.PubKeySize {
		return
	}

	pushKind := protocol.PushKind(body[0])
	target := body[1 : 1+protocol.PubKeySize]
	payload := body[1+protocol.PubKeySize:]
	targetHex := protocol.HexIdentity(target)

	if !s.allowProvisionalTarget(ctx, targetHex) {
		return
	}

	// If the recipient's key has been revoked, refuse to store or forward and tell
	// the sender instead. This is how a stranger the attacker messaged (while
	// impersonating the victim) lazily learns the identity is compromised: the
	// moment they reply, the server hands back the revocation notice.
	if revoked, err := s.server.repo.IsRevoked(ctx, targetHex); err != nil {
		s.logger.Error("failed to check recipient revocation", slog.Any("error", err), slog.String("to", targetHex))
		// Fail closed: if we can't verify revocation status, don't relay/store.
		return
	} else if revoked {
		if err := s.send(protocol.IdentityRevoked(target), false); err != nil {
			s.logger.Warn("failed to notify sender of revoked recipient", slog.Any("error", err))
		}
		return
	}

	msgID, err := s.server.repo.StoreMessage(ctx, requestID, s.identityHex, targetHex, payload)
	if err != nil {
		s.logger.Error("failed to store message", slog.Any("error", err), slog.String("to", targetHex))
		return
	}

	// notifyOffline emits the push appropriate for this message kind. It is only
	// called when the message could not be live-delivered, i.e. the recipient is
	// offline (or their session just died).
	notifyOffline := func() {
		switch pushKind {
		case protocol.PushKindCall:
			s.server.notifyIncomingCall(targetHex)
		case protocol.PushKindCallCancel:
			s.server.notifyCallCancelled(targetHex)
		case protocol.PushKindSilent:
			// Stored for later delivery; no push.
		default:
			s.server.notifyNewMessage(targetHex)
		}
	}

	if recipient := s.server.sessions.Find(targetHex); recipient != nil {
		if err := recipient.send(protocol.ForwardLive(s.identity, payload, msgID), false); err != nil {
			s.logger.Warn("live forward failed, message stays in db", slog.Any("error", err), slog.String("to", targetHex))
			notifyOffline()
		}
	} else {
		notifyOffline()
	}

	if err := s.send(protocol.ReceivedAck(requestID), false); err != nil {
		s.logger.Warn("failed to send sender acknowledgement", slog.Any("error", err))
	}
}

func (s *Session) handleMessageAck(ctx context.Context, body []byte) {
	if len(body) < protocol.DBMessageIDSize {
		return
	}
	msgID := int64(binary.BigEndian.Uint64(body[:protocol.DBMessageIDSize]))
	// Scoped to the acking session's own identity: a client must never be able
	// to delete another user's still-undelivered message just by guessing or
	// iterating sequential ids.
	if err := s.server.repo.DeleteMessageByID(ctx, msgID, s.identityHex); err != nil {
		s.logger.Warn("failed to delete acked message", slog.Any("error", err), slog.Int64("msg_id", msgID))
	}
}

// parseFCMTokenPayload extracts and validates the token carried by a
// RequestRegisterFCMToken body, shared by the live registration path and the
// provisional stash-for-later path.
func parseFCMTokenPayload(body []byte) (string, error) {
	var payload fcmTokenPayload
	if err := json.Unmarshal(body, &payload); err != nil {
		return "", fmt.Errorf("invalid fcm token payload: %w", err)
	}

	token := strings.TrimSpace(payload.Token)
	if token == "" || len(token) > protocol.MaxFCMTokenSize {
		return "", fmt.Errorf("invalid fcm token length: %d", len(token))
	}

	return token, nil
}

func (s *Session) handleRegisterFCMToken(ctx context.Context, requestID uint32, body []byte) {
	if !s.server.cfg.FCMEnabled() || s.server.notifier == nil {
		s.logger.Info("ignoring fcm token registration because fcm is disabled",
			slog.Uint64("request_id", uint64(requestID)))
		if err := s.send(protocol.FCMNotUsedAck(requestID), false); err != nil {
			s.logger.Warn("failed to send fcm disabled acknowledgement", slog.Any("error", err))
		}
		return
	}

	token, err := parseFCMTokenPayload(body)
	if err != nil {
		s.logger.Warn("rejected fcm token",
			slog.Uint64("request_id", uint64(requestID)),
			slog.Any("error", err))
		return
	}

	fingerprint := fcmTokenFingerprint(token)
	s.logger.Info("registering fcm token",
		slog.Uint64("request_id", uint64(requestID)),
		slog.Int("token_length", len(token)),
		slog.String("token_fingerprint", fingerprint))

	if err := s.server.repo.UpsertFCMToken(ctx, s.identityHex, token); err != nil {
		s.logger.Error("failed to register fcm token", slog.Any("error", err))
		return
	}

	s.logger.Info("fcm token registered",
		slog.Uint64("request_id", uint64(requestID)),
		slog.String("token_fingerprint", fingerprint))

	if err := s.send(protocol.FCMTokenReceivedAck(requestID), false); err != nil {
		s.logger.Warn("failed to send fcm registration acknowledgement", slog.Any("error", err))
		return
	}

	s.logger.Info("fcm registration acknowledgement sent", slog.Uint64("request_id", uint64(requestID)))
}

func fcmTokenFingerprint(token string) string {
	sum := sha256.Sum256([]byte(token))
	return base64.RawURLEncoding.EncodeToString(sum[:6])
}

func (s *Session) handleAudioFrame(ctx context.Context, body []byte) {
	if len(body) < protocol.PubKeySize {
		return
	}

	targetHex := protocol.HexIdentity(body[:protocol.PubKeySize])
	if !s.allowProvisionalStream(ctx, targetHex) {
		return
	}
	payload := body[protocol.PubKeySize:]

	if recipient := s.server.sessions.Find(targetHex); recipient != nil {
		if err := recipient.send(protocol.AudioFrame(s.identity, payload), true); err != nil {
			s.logger.Warn("failed to forward audio frame", slog.Any("error", err), slog.String("to", targetHex))
		}
	}
}

func (s *Session) handleReadedSignal(ctx context.Context, body []byte) {
	if len(body) < protocol.PubKeySize {
		return
	}

	targetHex := protocol.HexIdentity(body[:protocol.PubKeySize])
	if !s.allowProvisionalTarget(ctx, targetHex) {
		return
	}
	if recipient := s.server.sessions.Find(targetHex); recipient != nil {
		if err := recipient.send(protocol.ReadedSignal(s.identity), false); err != nil {
			s.logger.Warn("failed to forward readed signal", slog.Any("error", err), slog.String("to", targetHex))
			if err := s.server.repo.StoreSignal(ctx, uint8(protocol.ResponseReadedSignal), s.identityHex, targetHex, nil); err != nil {
				s.logger.Error("failed to store readed signal", slog.Any("error", err), slog.String("to", targetHex))
			}
		}
		return
	}

	if err := s.server.repo.StoreSignal(ctx, uint8(protocol.ResponseReadedSignal), s.identityHex, targetHex, nil); err != nil {
		s.logger.Error("failed to store readed signal", slog.Any("error", err), slog.String("to", targetHex))
	}
}

func (s *Session) handleReceivedSignal(ctx context.Context, body []byte) {
	if len(body) < protocol.PubKeySize+protocol.MessageIDSize {
		return
	}

	// received signals carry an attacker-controlled 8-byte message_id, so a
	// provisional client could otherwise store one row per distinct id — the
	// shared gate's deposit charge covers that (readed / session-removed
	// signals have an empty message_id and dedupe to a single row, so they
	// need no extra cap beyond the gate itself).
	targetHex := protocol.HexIdentity(body[:protocol.PubKeySize])
	if !s.allowProvisionalTarget(ctx, targetHex) {
		return
	}
	messageID := append([]byte(nil), body[protocol.PubKeySize:protocol.PubKeySize+protocol.MessageIDSize]...)

	if recipient := s.server.sessions.Find(targetHex); recipient != nil {
		if err := recipient.send(protocol.ReceivedSignal(s.identity, messageID), false); err != nil {
			s.logger.Warn("failed to forward received signal", slog.Any("error", err), slog.String("to", targetHex))
			if err := s.server.repo.StoreSignal(ctx, uint8(protocol.ResponseReceivedSignal), s.identityHex, targetHex, messageID); err != nil {
				s.logger.Error("failed to store received signal", slog.Any("error", err), slog.String("to", targetHex))
			}
		}
		return
	}

	if err := s.server.repo.StoreSignal(ctx, uint8(protocol.ResponseReceivedSignal), s.identityHex, targetHex, messageID); err != nil {
		s.logger.Error("failed to store received signal", slog.Any("error", err), slog.String("to", targetHex))
	}
}

func (s *Session) handleSessionRemoveSignal(ctx context.Context, body []byte) {
	if len(body) < protocol.PubKeySize {
		return
	}

	targetHex := protocol.HexIdentity(body[:protocol.PubKeySize])
	if !s.allowProvisionalTarget(ctx, targetHex) {
		return
	}
	if recipient := s.server.sessions.Find(targetHex); recipient != nil {
		if err := recipient.send(protocol.SessionRemovedSignal(s.identity), false); err != nil {
			s.logger.Warn("failed to forward session removed signal", slog.Any("error", err), slog.String("to", targetHex))
			if err := s.server.repo.StoreSignal(ctx, uint8(protocol.ResponseSessionRemoved), s.identityHex, targetHex, nil); err != nil {
				s.logger.Error("failed to store session removed signal", slog.Any("error", err), slog.String("to", targetHex))
			}
		}
		return
	}

	if err := s.server.repo.StoreSignal(ctx, uint8(protocol.ResponseSessionRemoved), s.identityHex, targetHex, nil); err != nil {
		s.logger.Error("failed to store session removed signal", slog.Any("error", err), slog.String("to", targetHex))
	}
}

func (s *Session) handleCountPrekeys(ctx context.Context, requestID uint32) {
	count, err := s.server.repo.CountPrekeys(ctx, s.identity)
	if err != nil {
		s.logger.Error("failed to count prekeys", slog.Any("error", err))
		count = 0
	}

	if count > protocol.MaxPrekeysPerID {
		count = protocol.MaxPrekeysPerID
	}

	if err := s.send(protocol.PrekeysCount(requestID, byte(count)), false); err != nil {
		s.logger.Warn("failed to send prekeys count", slog.Any("error", err))
	}
}

func (s *Session) handleSendPrekeys(ctx context.Context, body []byte) {
	// A revoked identity must not be able to repopulate its prekey pool and become
	// reachable for new sessions again.
	if revoked, err := s.server.repo.IsRevoked(ctx, s.identityHex); err != nil {
		s.logger.Error("failed to check own revocation before prekey upload", slog.Any("error", err))
		// Fail closed: if we can't verify revocation status, refuse the upload.
		return
	} else if revoked {
		return
	}

	existingCount, err := s.server.repo.CountPrekeys(ctx, s.identity)
	if err != nil {
		s.logger.Error("failed to count prekeys before insert", slog.Any("error", err))
		return
	}
	if existingCount >= protocol.MaxPrekeysPerID {
		return
	}

	var payloads []prekeyPayload
	if err := json.Unmarshal(body, &payloads); err != nil {
		s.logger.Warn("invalid prekeys JSON", slog.Any("error", err))
		return
	}

	overflow := existingCount + len(payloads) - protocol.MaxPrekeysPerID
	if overflow > 0 {
		payloads = payloads[overflow:]
	}

	for _, item := range payloads {
		record, ok := s.validatePrekey(item)
		if !ok {
			continue
		}

		if err := s.server.repo.StorePrekey(ctx, record); err != nil {
			s.logger.Error("failed to store prekey", slog.Any("error", err))
		}
	}
}

func (s *Session) validatePrekey(item prekeyPayload) (storage.PrekeyRecord, bool) {
	id, err := hex.DecodeString(item.ID)
	if err != nil {
		return storage.PrekeyRecord{}, false
	}
	dhPub, err := hex.DecodeString(item.DHPub)
	if err != nil {
		return storage.PrekeyRecord{}, false
	}
	pqPub, err := hex.DecodeString(item.PQPub)
	if err != nil {
		return storage.PrekeyRecord{}, false
	}
	edPub, err := hex.DecodeString(item.Ed25519Pub)
	if err != nil {
		return storage.PrekeyRecord{}, false
	}
	signature, err := hex.DecodeString(item.Signature)
	if err != nil {
		return storage.PrekeyRecord{}, false
	}

	if len(edPub) != protocol.PubKeySize || subtle.ConstantTimeCompare(edPub, s.identity) != 1 {
		return storage.PrekeyRecord{}, false
	}

	recomputedID := protocol.RecomputePrekeyID(dhPub, pqPub, edPub)
	if subtle.ConstantTimeCompare(id, recomputedID) != 1 {
		return storage.PrekeyRecord{}, false
	}

	signedData := make([]byte, 0, len(id)+len(dhPub)+len(pqPub)+len(edPub))
	signedData = append(signedData, id...)
	signedData = append(signedData, dhPub...)
	signedData = append(signedData, pqPub...)
	signedData = append(signedData, edPub...)

	if len(signature) != protocol.SignatureSize || !ed25519.Verify(ed25519.PublicKey(edPub), signedData, signature) {
		return storage.PrekeyRecord{}, false
	}

	return storage.PrekeyRecord{
		ID:            id,
		DHPubKey:      dhPub,
		PQPubKey:      pqPub,
		Ed25519PubKey: edPub,
		Signature:     signature,
	}, true
}

func (s *Session) handleGetPrekey(ctx context.Context, requestID uint32, body []byte) {
	// Block bootstrapping a new session against a revoked key. Everyone who tries
	// to start a fresh ratchet with the compromised identity is turned away with a
	// dedicated 410 code so the client can mark the contact as compromised.
	targetHex := protocol.HexIdentity(body)
	if revoked, err := s.server.repo.IsRevoked(ctx, targetHex); err != nil {
		s.logger.Error("failed to check prekey target revocation", slog.Any("error", err))
		// Fail closed: if we can't verify revocation status, refuse to dispense.
		return
	} else if revoked {
		payload := mustJSON(map[string]any{
			"error_code":    410,
			"error_message": "revoked",
		})
		if err := s.send(protocol.PrekeyResponse(requestID, payload), false); err != nil {
			s.logger.Warn("failed to send revoked prekey response", slog.Any("error", err))
		}
		return
	}

	record, err := s.server.repo.GetAnyPrekey(ctx, body)
	if err != nil {
		s.logger.Error("failed to fetch prekey", slog.Any("error", err))
		return
	}

	// Rate-limit actual dispensing per target identity so a peer cannot drain a
	// member's prekey pool. Checked only when a prekey exists, which keeps the
	// limiter map bounded by real members and does not penalise 404s.
	if record != nil && !s.server.allowPrekeyDispense(targetHex) {
		s.logger.Warn("prekey request rate-limited",
			slog.String("target", targetHex), slog.String("requester", s.identityHex))
		payload := mustJSON(map[string]any{
			"error_code":    429,
			"error_message": "prekey rate limit exceeded",
		})
		if err := s.send(protocol.PrekeyResponse(requestID, payload), false); err != nil {
			s.logger.Warn("failed to send rate-limit prekey response", slog.Any("error", err))
		}
		return
	}

	var payload []byte
	if record == nil {
		payload = mustJSON(map[string]any{
			"error_code":    404,
			"error_message": "not found",
		})
	} else {
		payload = mustJSON(map[string]any{
			"error_code":    0,
			"error_message": "ok",
			"id":            hex.EncodeToString(record.ID),
			"dh_pub":        hex.EncodeToString(record.DHPubKey),
			"pq_pub":        hex.EncodeToString(record.PQPubKey),
			"ed25519_pub":   hex.EncodeToString(record.Ed25519PubKey),
			"signature":     hex.EncodeToString(record.Signature),
		})
	}

	if err := s.send(protocol.PrekeyResponse(requestID, payload), false); err != nil {
		s.logger.Warn("failed to send prekey response", slog.Any("error", err))
		return
	}

	if record != nil {
		if err := s.server.repo.DeletePrekey(ctx, record.ID); err != nil {
			s.logger.Error("failed to delete dispensed prekey", slog.Any("error", err))
		}
	}
}

func (s *Session) send(data []byte, highPriority bool) error {
	// Oversized frames are split so a single big message can't monopolise the
	// connection. Each piece is itself below the threshold, so this never
	// recurses past one level.
	if len(data) > protocol.ChunkThreshold {
		return s.sendChunked(data, highPriority)
	}

	item := outboundItem{
		data: append([]byte(nil), data...),
		done: make(chan error, 1),
	}

	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return errors.New("session closed")
	}
	if highPriority {
		s.highQueue = append(s.highQueue, item)
	} else {
		s.lowQueue = append(s.lowQueue, item)
	}
	s.cond.Signal()
	s.mu.Unlock()

	select {
	case err := <-item.done:
		return err
	case <-s.done:
		return errors.New("session closed")
	}
}

// sendChunked splits an oversized frame into ChunkBegin / ChunkData* / ChunkEnd
// so it doesn't hog the connection: WS control frames (pings/pongs) and other
// sessions' small frames interleave between the pieces, and the client sees a
// steady byte stream instead of one long silence that its watchdog would read
// as a dead link. Each piece is below ChunkThreshold, so the send() calls here
// never re-chunk. Blocks until the whole transfer is written, exactly like a
// single send(); a partial write on failure just re-delivers on the next sync
// (the client dedupes / only ACKs a fully reassembled frame).
func (s *Session) sendChunked(data []byte, highPriority bool) error {
	transferID := s.transferCounter.Add(1)
	if err := s.send(protocol.ChunkBegin(transferID, uint32(len(data))), highPriority); err != nil {
		return err
	}
	for offset := 0; offset < len(data); offset += protocol.ChunkSize {
		end := offset + protocol.ChunkSize
		if end > len(data) {
			end = len(data)
		}
		if err := s.send(protocol.ChunkData(transferID, data[offset:end]), highPriority); err != nil {
			return err
		}
	}
	return s.send(protocol.ChunkEnd(transferID), highPriority)
}

func (s *Session) writeLoop() {
	for {
		item, ok := s.nextOutbound()
		if !ok {
			return
		}

		err := s.conn.WriteMessage(websocket.BinaryMessage, item.data)
		zeroBytes(item.data)
		item.done <- err
		close(item.done)

		if err != nil {
			s.shutdown(err)
			return
		}
	}
}

func (s *Session) nextOutbound() (outboundItem, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()

	for !s.closed && len(s.highQueue) == 0 && len(s.lowQueue) == 0 {
		s.cond.Wait()
	}

	if len(s.highQueue) > 0 {
		item := s.highQueue[0]
		s.highQueue[0] = outboundItem{}
		s.highQueue = s.highQueue[1:]
		return item, true
	}

	if len(s.lowQueue) > 0 {
		item := s.lowQueue[0]
		s.lowQueue[0] = outboundItem{}
		s.lowQueue = s.lowQueue[1:]
		return item, true
	}

	return outboundItem{}, false
}

func (s *Session) shutdown(cause error) {
	s.closeOnce.Do(func() {
		s.server.sessions.Remove(s.identityHex, s)

		s.mu.Lock()
		s.accessToken = ""
		s.closed = true
		pending := append(append([]outboundItem(nil), s.highQueue...), s.lowQueue...)
		s.highQueue = nil
		s.lowQueue = nil
		s.cond.Broadcast()
		s.mu.Unlock()

		for _, item := range pending {
			zeroBytes(item.data)
			item.done <- errors.New("session closed")
			close(item.done)
		}

		_ = s.conn.Close()
		close(s.done)

		if s.identityHex != "" {
			s.logger.Info("client disconnected")
		}
		if cause != nil {
			s.logger.Debug("session closed", slog.Any("error", cause))
		}
		zeroBytes(s.identity)
	})
}

func mustJSON(value any) []byte {
	data, err := json.Marshal(value)
	if err != nil {
		panic(err)
	}
	return data
}

func zeroBytes(data []byte) {
	for i := range data {
		data[i] = 0
	}
}

func generateAccessToken() (string, error) {
	raw := make([]byte, 32)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	defer zeroBytes(raw)

	return base64.RawURLEncoding.EncodeToString(raw), nil
}

func buildStoredSignalResponse(signalType uint8, from, messageID []byte) ([]byte, error) {
	switch protocol.ResponseType(signalType) {
	case protocol.ResponseReadedSignal:
		if len(messageID) != 0 {
			return nil, fmt.Errorf("readed signal message_id must be empty, got %d bytes", len(messageID))
		}
		return protocol.ReadedSignal(from), nil
	case protocol.ResponseReceivedSignal:
		if len(messageID) != protocol.MessageIDSize {
			return nil, fmt.Errorf("received signal message_id must be %d bytes, got %d", protocol.MessageIDSize, len(messageID))
		}
		return protocol.ReceivedSignal(from, messageID), nil
	case protocol.ResponseSessionRemoved:
		if len(messageID) != 0 {
			return nil, fmt.Errorf("session removed signal message_id must be empty, got %d bytes", len(messageID))
		}
		return protocol.SessionRemovedSignal(from), nil
	default:
		return nil, fmt.Errorf("unsupported stored signal type %d", signalType)
	}
}
