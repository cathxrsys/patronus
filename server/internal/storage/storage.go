package storage

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"strconv"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type Repository struct {
	pool   *pgxpool.Pool
	logger *slog.Logger
	schema string
}

type MessageRow struct {
	ID        int64
	RequestID uint32
	FromAddr  string
	ToAddr    string
	Payload   []byte
}

type SignalRow struct {
	SignalType uint8
	FromAddr   string
	ToAddr     string
	MessageID  []byte
}

type PrekeyRecord struct {
	ID            []byte
	DHPubKey      []byte
	PQPubKey      []byte
	Ed25519PubKey []byte
	Signature     []byte
}

type FCMTokenRecord struct {
	Token       string
	IdentityHex string
}

func NewRepository(ctx context.Context, connString string, schema string, poolSize int, logger *slog.Logger) (*Repository, error) {
	cfg, err := pgxpool.ParseConfig(connString)
	if err != nil {
		return nil, fmt.Errorf("parse pg config: %w", err)
	}
	if poolSize > 0 {
		cfg.MaxConns = int32(poolSize)
	}
	if schema != "" && schema != "public" {
		cfg.AfterConnect = func(ctx context.Context, conn *pgx.Conn) error {
			_, err := conn.Exec(ctx, fmt.Sprintf("SET search_path TO %s, public", schema))
			if err != nil {
				return fmt.Errorf("set search_path: %w", err)
			}
			return nil
		}
	}

	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		return nil, fmt.Errorf("create pg pool: %w", err)
	}

	if err := pool.Ping(ctx); err != nil {
		pool.Close()
		return nil, fmt.Errorf("ping postgres: %w", err)
	}

	return &Repository{pool: pool, logger: logger, schema: schema}, nil
}

func (r *Repository) Close() {
	r.pool.Close()
}

func (r *Repository) InitSchema(ctx context.Context) error {
	if r.schema != "" && r.schema != "public" {
		if _, err := r.pool.Exec(ctx, fmt.Sprintf("CREATE SCHEMA IF NOT EXISTS %s", r.schema)); err != nil {
			return fmt.Errorf("create schema: %w", err)
		}
	}

	queries := []string{
		`CREATE TABLE IF NOT EXISTS messages (
			id SERIAL PRIMARY KEY,
			request_id TEXT NOT NULL,
			from_addr TEXT NOT NULL,
			to_addr TEXT NOT NULL,
			payload BYTEA NOT NULL,
			created_at TIMESTAMP DEFAULT NOW()
		)`,
		`CREATE INDEX IF NOT EXISTS idx_messages_to_addr ON messages(to_addr)`,
		`CREATE TABLE IF NOT EXISTS signals (
			signal_type SMALLINT NOT NULL,
			from_addr TEXT NOT NULL,
			to_addr TEXT NOT NULL,
			message_id BYTEA NOT NULL DEFAULT ''::bytea,
			created_at TIMESTAMP DEFAULT NOW(),
			UNIQUE(signal_type, from_addr, to_addr, message_id)
		)`,
		`ALTER TABLE signals ADD COLUMN IF NOT EXISTS message_id BYTEA`,
		`DO $$
		BEGIN
			IF EXISTS (
				SELECT 1
				FROM information_schema.columns
				WHERE table_schema = current_schema()
				  AND table_name = 'signals'
				  AND column_name = 'payload'
			) THEN
				EXECUTE 'UPDATE signals SET message_id = payload WHERE message_id IS NULL AND payload IS NOT NULL';
				EXECUTE 'ALTER TABLE signals DROP COLUMN payload';
			END IF;
		END $$`,
		`UPDATE signals SET message_id = ''::bytea WHERE message_id IS NULL`,
		`ALTER TABLE signals ALTER COLUMN message_id SET DEFAULT ''::bytea`,
		`ALTER TABLE signals ALTER COLUMN message_id SET NOT NULL`,
		`DELETE FROM signals a
		 USING signals b
		 WHERE a.ctid < b.ctid
		   AND a.signal_type = b.signal_type
		   AND a.from_addr = b.from_addr
		   AND a.to_addr = b.to_addr
		   AND a.message_id = b.message_id`,
		`CREATE UNIQUE INDEX IF NOT EXISTS idx_signals_unique ON signals(signal_type, from_addr, to_addr, message_id)`,
		`CREATE INDEX IF NOT EXISTS idx_signals_to_addr ON signals(to_addr)`,
		`CREATE TABLE IF NOT EXISTS prekeys (
			id BYTEA PRIMARY KEY NOT NULL,
			dh_pubkey BYTEA NOT NULL,
			pq_pubkey BYTEA NOT NULL,
			ed25519_pubkey BYTEA NOT NULL,
			signature BYTEA NOT NULL
		)`,
		`CREATE TABLE IF NOT EXISTS files (
			id BIGINT PRIMARY KEY NOT NULL,
			size BIGINT NOT NULL,
			sha256 TEXT NOT NULL,
			storage_path TEXT NOT NULL UNIQUE,
			uploaded_at TIMESTAMP NOT NULL DEFAULT NOW()
		)`,
		`CREATE INDEX IF NOT EXISTS idx_files_uploaded_at ON files(uploaded_at)`,
		`CREATE TABLE IF NOT EXISTS file_recipients (
			file_id BIGINT NOT NULL REFERENCES files(id) ON DELETE CASCADE,
			recipient_addr TEXT NOT NULL,
			PRIMARY KEY(file_id, recipient_addr)
		)`,
		`CREATE INDEX IF NOT EXISTS idx_file_recipients_recipient_addr ON file_recipients(recipient_addr)`,
		`CREATE TABLE IF NOT EXISTS fcm_tokens (
			token TEXT PRIMARY KEY NOT NULL,
			identity_hex TEXT NOT NULL,
			created_at TIMESTAMP NOT NULL DEFAULT NOW(),
			updated_at TIMESTAMP NOT NULL DEFAULT NOW()
		)`,
		`CREATE INDEX IF NOT EXISTS idx_fcm_tokens_identity_hex ON fcm_tokens(identity_hex)`,
		`CREATE TABLE IF NOT EXISTS revoked_identities (
			identity_hex TEXT PRIMARY KEY NOT NULL,
			signature BYTEA NOT NULL,
			created_at TIMESTAMP NOT NULL DEFAULT NOW()
		)`,
		`CREATE TABLE IF NOT EXISTS whitelist (
			identity_hex TEXT PRIMARY KEY NOT NULL,
			created_at TIMESTAMP NOT NULL DEFAULT NOW()
		)`,
	}

	for _, query := range queries {
		if _, err := r.pool.Exec(ctx, query); err != nil {
			return fmt.Errorf("exec schema query: %w", err)
		}
	}

	return nil
}

func (r *Repository) StoreMessage(ctx context.Context, requestID uint32, fromAddr, toAddr string, payload []byte) (int64, error) {
	var id int64
	err := r.pool.QueryRow(
		ctx,
		`INSERT INTO messages (request_id, from_addr, to_addr, payload) VALUES ($1, $2, $3, $4) RETURNING id`,
		strconv.FormatUint(uint64(requestID), 10), fromAddr, toAddr, payload,
	).Scan(&id)
	if err != nil {
		return 0, fmt.Errorf("insert message: %w", err)
	}
	return id, nil
}

func (r *Repository) FetchMessages(ctx context.Context, toAddr string, limit int) ([]MessageRow, error) {
	rows, err := r.pool.Query(
		ctx,
		`SELECT id, request_id, from_addr, to_addr, payload FROM messages WHERE to_addr = $1 ORDER BY created_at ASC, id ASC LIMIT $2`,
		toAddr, limit,
	)
	if err != nil {
		return nil, fmt.Errorf("fetch messages: %w", err)
	}
	defer rows.Close()

	result := make([]MessageRow, 0)
	for rows.Next() {
		var requestIDText string
		var row MessageRow
		if err := rows.Scan(&row.ID, &requestIDText, &row.FromAddr, &row.ToAddr, &row.Payload); err != nil {
			return nil, fmt.Errorf("scan message row: %w", err)
		}

		parsed, err := strconv.ParseUint(requestIDText, 10, 32)
		if err != nil {
			return nil, fmt.Errorf("parse request id %q: %w", requestIDText, err)
		}

		row.RequestID = uint32(parsed)
		result = append(result, row)
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate message rows: %w", err)
	}

	return result, nil
}

// FetchMessagesAfter streams a recipient's stored messages in ascending id
// order starting strictly after afterID. Offline sync pages through the backlog
// with this cursor instead of deleting as it streams: deletion is driven by the
// recipient's per-message ACK (see handleMessageAck), so an interrupted sync
// never drops a message that was sent but not yet confirmed received — it simply
// re-delivers on the next sync and the client dedupes by message id. Because id
// is strictly increasing, the cursor guarantees the sync loop still terminates
// even though nothing is deleted inside it.
func (r *Repository) FetchMessagesAfter(ctx context.Context, toAddr string, afterID int64, limit int) ([]MessageRow, error) {
	rows, err := r.pool.Query(
		ctx,
		`SELECT id, request_id, from_addr, to_addr, payload FROM messages WHERE to_addr = $1 AND id > $2 ORDER BY id ASC LIMIT $3`,
		toAddr, afterID, limit,
	)
	if err != nil {
		return nil, fmt.Errorf("fetch messages after: %w", err)
	}
	defer rows.Close()

	result := make([]MessageRow, 0)
	for rows.Next() {
		var requestIDText string
		var row MessageRow
		if err := rows.Scan(&row.ID, &requestIDText, &row.FromAddr, &row.ToAddr, &row.Payload); err != nil {
			return nil, fmt.Errorf("scan message row: %w", err)
		}

		parsed, err := strconv.ParseUint(requestIDText, 10, 32)
		if err != nil {
			return nil, fmt.Errorf("parse request id %q: %w", requestIDText, err)
		}

		row.RequestID = uint32(parsed)
		result = append(result, row)
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate message rows: %w", err)
	}

	return result, nil
}

func (r *Repository) DeleteMessageByID(ctx context.Context, messageID int64, toAddr string) error {
	_, err := r.pool.Exec(
		ctx,
		`DELETE FROM messages WHERE id = $1 AND to_addr = $2`,
		messageID, toAddr,
	)
	if err != nil {
		return fmt.Errorf("delete message: %w", err)
	}
	return nil
}

// DeleteMessagesByIDs removes a batch of this recipient's messages in one round
// trip. Used by the offline sync to delete a streamed batch at once instead of
// one DELETE per message. to_addr is part of the predicate so a caller can only
// ever delete its own rows.
func (r *Repository) DeleteMessagesByIDs(ctx context.Context, ids []int64, toAddr string) error {
	if len(ids) == 0 {
		return nil
	}
	_, err := r.pool.Exec(
		ctx,
		`DELETE FROM messages WHERE to_addr = $1 AND id = ANY($2)`,
		toAddr, ids,
	)
	if err != nil {
		return fmt.Errorf("delete messages: %w", err)
	}
	return nil
}

func (r *Repository) StoreSignal(ctx context.Context, signalType uint8, fromAddr, toAddr string, messageID []byte) error {
	normalizedMessageID := normalizeSignalMessageID(messageID)

	_, err := r.pool.Exec(
		ctx,
		`INSERT INTO signals (signal_type, from_addr, to_addr, message_id) VALUES ($1, $2, $3, $4) ON CONFLICT DO NOTHING`,
		signalType, fromAddr, toAddr, normalizedMessageID,
	)
	if err != nil {
		return fmt.Errorf("insert signal: %w", err)
	}
	return nil
}

func (r *Repository) FetchSignals(ctx context.Context, toAddr string, limit int) ([]SignalRow, error) {
	rows, err := r.pool.Query(
		ctx,
		`SELECT signal_type, from_addr, to_addr, message_id
		 FROM signals
		 WHERE to_addr = $1
		 GROUP BY signal_type, from_addr, to_addr, message_id
		 ORDER BY MIN(created_at) ASC
		 LIMIT $2`,
		toAddr, limit,
	)
	if err != nil {
		return nil, fmt.Errorf("fetch signals: %w", err)
	}
	defer rows.Close()

	result := make([]SignalRow, 0)
	for rows.Next() {
		var row SignalRow
		var signalType int16
		if err := rows.Scan(&signalType, &row.FromAddr, &row.ToAddr, &row.MessageID); err != nil {
			return nil, fmt.Errorf("scan signal row: %w", err)
		}

		if signalType < 0 || signalType > 255 {
			return nil, fmt.Errorf("signal type out of range: %d", signalType)
		}

		row.SignalType = uint8(signalType)
		result = append(result, row)
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate signal rows: %w", err)
	}

	return result, nil
}

func (r *Repository) DeleteSignal(ctx context.Context, signalType uint8, fromAddr, toAddr string, messageID []byte) error {
	normalizedMessageID := normalizeSignalMessageID(messageID)

	_, err := r.pool.Exec(
		ctx,
		`DELETE FROM signals WHERE signal_type = $1 AND from_addr = $2 AND to_addr = $3 AND message_id = $4`,
		signalType, fromAddr, toAddr, normalizedMessageID,
	)
	if err != nil {
		return fmt.Errorf("delete signal: %w", err)
	}
	return nil
}

func normalizeSignalMessageID(messageID []byte) []byte {
	if len(messageID) == 0 {
		return []byte{}
	}
	return append([]byte(nil), messageID...)
}

func (r *Repository) UpsertFCMToken(ctx context.Context, identityHex, token string) error {
	_, err := r.pool.Exec(
		ctx,
		`INSERT INTO fcm_tokens (token, identity_hex)
		 VALUES ($1, $2)
		 ON CONFLICT (token) DO UPDATE
		 SET identity_hex = EXCLUDED.identity_hex,
		     updated_at = NOW()`,
		token, identityHex,
	)
	if err != nil {
		return fmt.Errorf("upsert fcm token: %w", err)
	}
	return nil
}

func (r *Repository) ListFCMTokens(ctx context.Context, identityHex string) ([]string, error) {
	rows, err := r.pool.Query(
		ctx,
		`SELECT token FROM fcm_tokens WHERE identity_hex = $1 ORDER BY updated_at DESC, created_at DESC`,
		identityHex,
	)
	if err != nil {
		return nil, fmt.Errorf("list fcm tokens: %w", err)
	}
	defer rows.Close()

	tokens := make([]string, 0)
	for rows.Next() {
		var token string
		if err := rows.Scan(&token); err != nil {
			return nil, fmt.Errorf("scan fcm token: %w", err)
		}
		tokens = append(tokens, token)
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate fcm tokens: %w", err)
	}

	return tokens, nil
}

func (r *Repository) DeleteFCMToken(ctx context.Context, token string) error {
	_, err := r.pool.Exec(ctx, `DELETE FROM fcm_tokens WHERE token = $1`, token)
	if err != nil {
		return fmt.Errorf("delete fcm token: %w", err)
	}
	return nil
}

func (r *Repository) CountPrekeys(ctx context.Context, ed25519PubKey []byte) (int, error) {
	var count int
	if err := r.pool.QueryRow(ctx, `SELECT COUNT(*) FROM prekeys WHERE ed25519_pubkey = $1`, ed25519PubKey).Scan(&count); err != nil {
		return 0, fmt.Errorf("count prekeys: %w", err)
	}
	return count, nil
}

func (r *Repository) StorePrekey(ctx context.Context, record PrekeyRecord) error {
	_, err := r.pool.Exec(
		ctx,
		`INSERT INTO prekeys (id, dh_pubkey, pq_pubkey, ed25519_pubkey, signature) VALUES ($1, $2, $3, $4, $5)`,
		record.ID, record.DHPubKey, record.PQPubKey, record.Ed25519PubKey, record.Signature,
	)
	if err != nil {
		return fmt.Errorf("insert prekey: %w", err)
	}
	return nil
}

func (r *Repository) GetAnyPrekey(ctx context.Context, ed25519PubKey []byte) (*PrekeyRecord, error) {
	var record PrekeyRecord
	err := r.pool.QueryRow(
		ctx,
		`SELECT id, dh_pubkey, pq_pubkey, ed25519_pubkey, signature FROM prekeys WHERE ed25519_pubkey = $1 LIMIT 1`,
		ed25519PubKey,
	).Scan(&record.ID, &record.DHPubKey, &record.PQPubKey, &record.Ed25519PubKey, &record.Signature)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, nil
		}
		return nil, fmt.Errorf("query prekey: %w", err)
	}
	return &record, nil
}

func (r *Repository) DeletePrekey(ctx context.Context, id []byte) error {
	_, err := r.pool.Exec(ctx, `DELETE FROM prekeys WHERE id = $1`, id)
	if err != nil {
		return fmt.Errorf("delete prekey: %w", err)
	}
	return nil
}

func (r *Repository) DeletePrekeysForIdentity(ctx context.Context, ed25519PubKey []byte) error {
	_, err := r.pool.Exec(ctx, `DELETE FROM prekeys WHERE ed25519_pubkey = $1`, ed25519PubKey)
	if err != nil {
		return fmt.Errorf("delete prekeys for identity: %w", err)
	}
	return nil
}

// StoreRevocation records a permanent tombstone for a compromised identity key.
// It is idempotent: revocation is monotonic, so a repeated certificate for an
// already-revoked key is a no-op.
func (r *Repository) StoreRevocation(ctx context.Context, identityHex string, signature []byte) error {
	_, err := r.pool.Exec(
		ctx,
		`INSERT INTO revoked_identities (identity_hex, signature) VALUES ($1, $2)
		 ON CONFLICT (identity_hex) DO NOTHING`,
		identityHex, signature,
	)
	if err != nil {
		return fmt.Errorf("insert revocation: %w", err)
	}
	return nil
}

// IsRevoked reports whether the given identity has published a revocation
// certificate. Once true it never becomes false again.
func (r *Repository) IsRevoked(ctx context.Context, identityHex string) (bool, error) {
	var exists bool
	if err := r.pool.QueryRow(
		ctx,
		`SELECT EXISTS(SELECT 1 FROM revoked_identities WHERE identity_hex = $1)`,
		identityHex,
	).Scan(&exists); err != nil {
		return false, fmt.Errorf("check revocation: %w", err)
	}
	return exists, nil
}

// SeedWhitelist inserts the operator-configured root identities into the
// whitelist. Idempotent: existing rows are left untouched, so it is safe to run
// on every startup.
func (r *Repository) SeedWhitelist(ctx context.Context, identityHexes []string) error {
	for _, identityHex := range identityHexes {
		if identityHex == "" {
			continue
		}
		if err := r.AddToWhitelist(ctx, identityHex); err != nil {
			return err
		}
	}
	return nil
}

// AddToWhitelist admits an identity to the server. Idempotent.
func (r *Repository) AddToWhitelist(ctx context.Context, identityHex string) error {
	_, err := r.pool.Exec(
		ctx,
		`INSERT INTO whitelist (identity_hex) VALUES ($1) ON CONFLICT (identity_hex) DO NOTHING`,
		identityHex,
	)
	if err != nil {
		return fmt.Errorf("insert whitelist entry: %w", err)
	}
	return nil
}

// IsWhitelisted reports whether the given identity has been admitted.
func (r *Repository) IsWhitelisted(ctx context.Context, identityHex string) (bool, error) {
	var exists bool
	if err := r.pool.QueryRow(
		ctx,
		`SELECT EXISTS(SELECT 1 FROM whitelist WHERE identity_hex = $1)`,
		identityHex,
	).Scan(&exists); err != nil {
		return false, fmt.Errorf("check whitelist: %w", err)
	}
	return exists, nil
}

// CountWhitelist returns the number of admitted identities, used to enforce the
// optional flat member cap.
func (r *Repository) CountWhitelist(ctx context.Context) (int, error) {
	var count int
	if err := r.pool.QueryRow(ctx, `SELECT COUNT(*) FROM whitelist`).Scan(&count); err != nil {
		return 0, fmt.Errorf("count whitelist: %w", err)
	}
	return count, nil
}
