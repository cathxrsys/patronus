package config

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"unicode"

	"github.com/jackc/pgx/v5"
)

type firebaseClientConfig struct {
	ProjectInfo struct {
		ProjectID string `json:"project_id"`
	} `json:"project_info"`
	Clients []struct {
		ClientInfo struct {
			MobileSDKAppID string `json:"mobilesdk_app_id"`
		} `json:"client_info"`
		APIKeys []struct {
			CurrentKey string `json:"current_key"`
		} `json:"api_key"`
	} `json:"client"`
}

type Config struct {
	ID                       string `json:"id"`
	IP                       string `json:"ip"`
	Port                     int    `json:"port"`
	SSLCertFile              string `json:"ssl_cert_file"`
	SSLKeyFile               string `json:"ssl_key_file"`
	DBConnectionString       string `json:"db_connection_string"`
	DBSchema                 string `json:"db_schema"`
	DBConnectionPoolSize     int    `json:"db_connection_pool_size"`
	FileStoragePath          string `json:"file_storage_path"`
	FileTTLSeconds           int64  `json:"file_ttl_seconds"`
	MaxFileSize              int64  `json:"max_file_size"`
	MaxStorageSize           int64  `json:"max_storage_size"`
	FCMProjectID             string `json:"fcm_project_id"`
	FCMAPIKey                string `json:"fcm_api_key"`
	FCMAppID                 string `json:"fcm_app_id"`
	FCMClientConfigFile      string `json:"fcm_client_config_file"`
	FCMServiceAccountFile    string `json:"fcm_service_account_file"`
	FCMRequestTimeoutSeconds int    `json:"fcm_request_timeout_seconds"`
	Debug                    bool   `json:"debug"`

	// Members is the allowlist of ed25519 identity public keys (hex) permitted
	// to use this server. It is seeded by the operator with their own key(s) on
	// deploy. When empty the membership gate is DISABLED (any authenticated key
	// is accepted) so a fresh deployment is not bricked; the operator turns the
	// gate on simply by listing one or more members.
	Members []string `json:"members"`

	// Reserved tree-growth limits, wired now and applied once transitive
	// enrollment lands. -1 means unlimited (the default). TreeMaxMembers is a
	// flat cap on the member-set size; TreeMaxDepth / TreeMaxFanout need the
	// invite tree (parent edges) which v1 does not track yet.
	TreeMaxMembers int `json:"tree_max_members"`
	TreeMaxDepth   int `json:"tree_max_depth"`
	TreeMaxFanout  int `json:"tree_max_fanout"`

	// PrekeyRatePerMinute caps how many prekeys the server will dispense per
	// target identity per minute, so a peer cannot drain a member's prekey pool.
	// 0 or negative disables the limit.
	PrekeyRatePerMinute int `json:"prekey_rate_per_minute"`

	// ProvisionalDepositBudget caps how many messages a not-yet-enrolled
	// (provisional) session may deposit before it is promoted to a member. This
	// stops a client with a wrong/forged invite — whose messages never decrypt —
	// from flooding a member's queue and push notifications. A legitimate
	// onboarding needs only a couple (session_request + accountInfo). -1 disables
	// the limit.
	ProvisionalDepositBudget int `json:"provisional_deposit_budget"`
}

var ErrHelp = errors.New("help requested")

func Default() Config {
	return Config{
		ID:                       "default",
		IP:                       "0.0.0.0",
		Port:                     443,
		SSLCertFile:              "./server.crt",
		SSLKeyFile:               "./server.key",
		DBConnectionString:       "host=127.0.0.1 port=5432 dbname=coreserver user=coreuser password=corepass",
		DBSchema:                 "coreserver_{id}",
		DBConnectionPoolSize:     16,
		FileStoragePath:          "/srv/coreserver/files/",
		FileTTLSeconds:           7 * 24 * 60 * 60,
		MaxFileSize:              0,
		MaxStorageSize:           0,
		FCMRequestTimeoutSeconds: 10,
		Debug:                    false,
		TreeMaxMembers:           -1,
		TreeMaxDepth:             -1,
		TreeMaxFanout:            -1,
		PrekeyRatePerMinute:      30,
		ProvisionalDepositBudget: 8,
	}
}

// MembershipGateEnabled reports whether the server should reject identities
// that are not on the allowlist. The gate is opt-in: it activates only once
// the operator lists at least one member.
func (cfg Config) MembershipGateEnabled() bool {
	return len(cfg.NormalizedMembers()) > 0
}

// NormalizedMembers returns the member allowlist as a lowercased, trimmed set
// for O(1) lookup. Blank entries are skipped.
func (cfg Config) NormalizedMembers() map[string]struct{} {
	set := make(map[string]struct{}, len(cfg.Members))
	for _, m := range cfg.NormalizedMemberList() {
		set[m] = struct{}{}
	}
	return set
}

// NormalizedMemberList returns the deduplicated, lowercased root member keys as
// a slice, used to seed the whitelist table on startup.
func (cfg Config) NormalizedMemberList() []string {
	seen := make(map[string]struct{}, len(cfg.Members))
	list := make([]string, 0, len(cfg.Members))
	for _, m := range cfg.Members {
		normalized := strings.ToLower(strings.TrimSpace(m))
		if normalized == "" {
			continue
		}
		if _, dup := seen[normalized]; dup {
			continue
		}
		seen[normalized] = struct{}{}
		list = append(list, normalized)
	}
	return list
}

func ParseArgs(args []string, stdout io.Writer, stderr io.Writer) (string, Config, error) {
	configPath := "./config.json"
	cfg := Default()

	flags := flag.NewFlagSet("server", flag.ContinueOnError)
	flags.SetOutput(stderr)
	flags.StringVar(&configPath, "config", configPath, "path to config JSON file")
	flags.StringVar(&cfg.ID, "id", cfg.ID, "instance id used to isolate database and storage")
	flags.StringVar(&cfg.IP, "ip", cfg.IP, "TLS listen IP address")
	flags.IntVar(&cfg.Port, "port", cfg.Port, "TLS listen port")
	flags.StringVar(&cfg.SSLCertFile, "ssl-cert-file", cfg.SSLCertFile, "path to TLS certificate file")
	flags.StringVar(&cfg.SSLKeyFile, "ssl-key-file", cfg.SSLKeyFile, "path to TLS private key file")
	flags.StringVar(&cfg.DBConnectionString, "db-connection-string", cfg.DBConnectionString, "PostgreSQL connection string")
	flags.StringVar(&cfg.DBSchema, "db-schema", cfg.DBSchema, "PostgreSQL schema used by this server instance")
	flags.IntVar(&cfg.DBConnectionPoolSize, "db-connection-pool-size", cfg.DBConnectionPoolSize, "PostgreSQL pool size")
	flags.StringVar(&cfg.FileStoragePath, "file-storage-path", cfg.FileStoragePath, "directory for file storage")
	flags.Int64Var(&cfg.FileTTLSeconds, "file-ttl-seconds", cfg.FileTTLSeconds, "file retention time in seconds")
	flags.Int64Var(&cfg.MaxFileSize, "max-file-size", cfg.MaxFileSize, "maximum uploaded file size in bytes, 0 disables the limit")
	flags.Int64Var(&cfg.MaxStorageSize, "max-storage-size", cfg.MaxStorageSize, "maximum total storage size in bytes, 0 disables the limit")
	flags.StringVar(&cfg.FCMProjectID, "fcm-project-id", cfg.FCMProjectID, "Firebase project id used for push notifications")
	flags.StringVar(&cfg.FCMAPIKey, "fcm-api-key", cfg.FCMAPIKey, "Firebase Web API key for this deployment")
	flags.StringVar(&cfg.FCMAppID, "fcm-app-id", cfg.FCMAppID, "Firebase app id used by the client")
	flags.StringVar(&cfg.FCMClientConfigFile, "fcm-client-config-file", cfg.FCMClientConfigFile, "path to public Firebase client config file served to clients, e.g. google-services.json")
	flags.StringVar(&cfg.FCMServiceAccountFile, "fcm-service-account-file", cfg.FCMServiceAccountFile, "path to Firebase service account JSON file")
	flags.IntVar(&cfg.FCMRequestTimeoutSeconds, "fcm-request-timeout-seconds", cfg.FCMRequestTimeoutSeconds, "timeout in seconds for outbound FCM requests")
	flags.IntVar(&cfg.TreeMaxMembers, "tree-max-members", cfg.TreeMaxMembers, "maximum member-set size, -1 disables the limit")
	flags.IntVar(&cfg.TreeMaxDepth, "tree-max-depth", cfg.TreeMaxDepth, "reserved invite-tree depth limit, -1 disables the limit")
	flags.IntVar(&cfg.TreeMaxFanout, "tree-max-fanout", cfg.TreeMaxFanout, "reserved invite-tree fan-out limit, -1 disables the limit")
	flags.IntVar(&cfg.PrekeyRatePerMinute, "prekey-rate-per-minute", cfg.PrekeyRatePerMinute, "max prekeys dispensed per target identity per minute, 0 disables the limit")
	flags.IntVar(&cfg.ProvisionalDepositBudget, "provisional-deposit-budget", cfg.ProvisionalDepositBudget, "max messages a not-yet-enrolled session may deposit, -1 disables the limit")
	flags.BoolVar(&cfg.Debug, "debug", cfg.Debug, "enable verbose debug logging")
	flags.Usage = func() {
		fmt.Fprintf(stdout, "Usage: %s [options]\n\n", flags.Name())
		fmt.Fprintln(stdout, "If --config points to an existing file, values are loaded from it.")
		fmt.Fprintln(stdout, "If the file does not exist, the server falls back to command-line flags and defaults.")
		fmt.Fprintln(stdout, "Database schema and file storage are isolated for the resolved instance id.")
		fmt.Fprintln(stdout)
		flags.SetOutput(stdout)
		flags.PrintDefaults()
		flags.SetOutput(stderr)
	}

	if err := flags.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return configPath, Config{}, ErrHelp
		}
		return configPath, Config{}, err
	}

	if flags.NArg() > 0 {
		return configPath, Config{}, fmt.Errorf("unexpected positional arguments: %v", flags.Args())
	}

	return configPath, cfg, nil
}

func Load(path string, fallback Config, logger *slog.Logger) (Config, error) {
	cfg := fallback

	data, err := os.ReadFile(path)
	if err != nil {
		logger.Warn("config file not found, using command-line values and defaults", slog.String("path", path))
		return resolveDerived(cfg)
	}

	if err := json.Unmarshal(data, &cfg); err != nil {
		logger.Error("failed to parse config, using command-line values and defaults", slog.String("path", path), slog.Any("error", err))
		return resolveDerived(fallback)
	}

	resolved, err := resolveDerived(cfg)
	if err != nil {
		return Config{}, err
	}

	logger.Info("config loaded",
		slog.String("path", path),
		slog.String("id", resolved.ID),
		slog.String("ip", resolved.IP),
		slog.Int("port", resolved.Port),
		slog.String("ssl_cert_file", resolved.SSLCertFile),
		slog.String("ssl_key_file", resolved.SSLKeyFile),
		slog.String("db_schema", resolved.DBSchema),
		slog.Int("db_connection_pool_size", resolved.DBConnectionPoolSize),
		slog.String("file_storage_path", resolved.FileStoragePath),
		slog.Int64("file_ttl_seconds", resolved.FileTTLSeconds),
		slog.Int64("max_file_size", resolved.MaxFileSize),
		slog.Int64("max_storage_size", resolved.MaxStorageSize),
		slog.Bool("fcm_enabled", resolved.FCMEnabled()),
		slog.String("fcm_project_id", resolved.FCMProjectID),
		slog.String("fcm_app_id", resolved.FCMAppID),
		slog.String("fcm_client_config_file", resolved.FCMClientConfigFile),
		slog.String("fcm_service_account_file", resolved.FCMServiceAccountFile),
		slog.Int("fcm_request_timeout_seconds", resolved.FCMRequestTimeoutSeconds),
		slog.Bool("membership_gate_enabled", resolved.MembershipGateEnabled()),
		slog.Int("member_count", len(resolved.NormalizedMembers())),
		slog.Int("tree_max_members", resolved.TreeMaxMembers),
		slog.Int("prekey_rate_per_minute", resolved.PrekeyRatePerMinute),
		slog.Int("provisional_deposit_budget", resolved.ProvisionalDepositBudget),
		slog.Bool("debug", resolved.Debug),
	)

	if !resolved.MembershipGateEnabled() {
		logger.Warn("membership gate is DISABLED — any authenticated identity is accepted; add public keys to \"members\" in the config to enable it")
	}

	return resolved, nil
}

func resolveDerived(cfg Config) (Config, error) {
	if strings.TrimSpace(cfg.IP) == "" {
		cfg.IP = Default().IP
	}

	normalizedID, err := normalizeID(cfg.ID)
	if err != nil {
		return Config{}, err
	}
	cfg.ID = normalizedID

	if cfg.ID == "" {
		if cfg.DBSchema == "" {
			cfg.DBSchema = "public"
		}
		cfg.DBSchema, err = normalizeSchema(cfg.DBSchema)
		if err != nil {
			return Config{}, err
		}
		return validateFCMConfig(cfg)
	}

	cfg.FileStoragePath = applyInstancePath(cfg.FileStoragePath, cfg.ID)
	cfg.DBSchema, err = resolveSchema(cfg.DBSchema, cfg.ID)
	if err != nil {
		return Config{}, err
	}
	cfg.DBConnectionString, err = applyInstanceDB(cfg.DBConnectionString)
	if err != nil {
		return Config{}, err
	}

	return validateFCMConfig(cfg)
}

func (cfg Config) FCMEnabled() bool {
	return strings.TrimSpace(cfg.FCMClientConfigFile) != "" &&
		strings.TrimSpace(cfg.FCMServiceAccountFile) != ""
}

func validateFCMConfig(cfg Config) (Config, error) {
	cfg.FCMProjectID = strings.TrimSpace(cfg.FCMProjectID)
	cfg.FCMAPIKey = strings.TrimSpace(cfg.FCMAPIKey)
	cfg.FCMAppID = strings.TrimSpace(cfg.FCMAppID)
	cfg.FCMClientConfigFile = strings.TrimSpace(cfg.FCMClientConfigFile)
	if cfg.FCMClientConfigFile != "" {
		cfg.FCMClientConfigFile = filepath.Clean(cfg.FCMClientConfigFile)
	}
	cfg.FCMServiceAccountFile = strings.TrimSpace(cfg.FCMServiceAccountFile)
	if cfg.FCMServiceAccountFile != "" {
		cfg.FCMServiceAccountFile = filepath.Clean(cfg.FCMServiceAccountFile)
	}
	if cfg.FCMRequestTimeoutSeconds <= 0 {
		cfg.FCMRequestTimeoutSeconds = Default().FCMRequestTimeoutSeconds
	}

	if cfg.FCMClientConfigFile != "" {
		parsedMetadata, err := parseFirebaseClientMetadata(cfg.FCMClientConfigFile)
		if err != nil {
			return Config{}, err
		}
		if cfg.FCMProjectID == "" {
			cfg.FCMProjectID = parsedMetadata.ProjectID
		}
		if cfg.FCMAPIKey == "" {
			cfg.FCMAPIKey = parsedMetadata.APIKey
		}
		if cfg.FCMAppID == "" {
			cfg.FCMAppID = parsedMetadata.AppID
		}
	}

	if !cfg.FCMEnabled() {
		return cfg, nil
	}

	if cfg.FCMClientConfigFile == cfg.FCMServiceAccountFile {
		return Config{}, fmt.Errorf("fcm_client_config_file must not point to the same file as fcm_service_account_file")
	}
	if cfg.FCMProjectID == "" {
		return Config{}, fmt.Errorf("unable to resolve fcm_project_id from fcm_client_config_file")
	}

	return cfg, nil
}

type firebaseClientMetadata struct {
	ProjectID string
	APIKey    string
	AppID     string
}

func parseFirebaseClientMetadata(path string) (firebaseClientMetadata, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return firebaseClientMetadata{}, fmt.Errorf("read fcm_client_config_file: %w", err)
	}

	var parsed firebaseClientConfig
	if err := json.Unmarshal(data, &parsed); err != nil {
		return firebaseClientMetadata{}, fmt.Errorf("parse fcm_client_config_file: %w", err)
	}

	metadata := firebaseClientMetadata{
		ProjectID: strings.TrimSpace(parsed.ProjectInfo.ProjectID),
	}
	for _, client := range parsed.Clients {
		if metadata.AppID == "" {
			metadata.AppID = strings.TrimSpace(client.ClientInfo.MobileSDKAppID)
		}
		if metadata.APIKey == "" {
			for _, apiKey := range client.APIKeys {
				if strings.TrimSpace(apiKey.CurrentKey) == "" {
					continue
				}
				metadata.APIKey = strings.TrimSpace(apiKey.CurrentKey)
				break
			}
		}
		if metadata.AppID != "" && metadata.APIKey != "" {
			break
		}
	}

	return metadata, nil
}

func normalizeID(id string) (string, error) {
	id = strings.TrimSpace(id)
	if id == "" {
		return "default", nil
	}

	for _, r := range id {
		if unicode.IsLetter(r) || unicode.IsDigit(r) || r == '_' || r == '-' {
			continue
		}
		return "", fmt.Errorf("invalid id %q: only letters, digits, '_' and '-' are allowed", id)
	}

	return id, nil
}

func applyInstancePath(basePath string, id string) string {
	if strings.Contains(basePath, "{id}") {
		return filepath.Clean(strings.ReplaceAll(basePath, "{id}", id))
	}

	return filepath.Join(basePath, id)
}

func applyInstanceDB(connString string) (string, error) {
	parsed, err := pgx.ParseConfig(connString)
	if err != nil {
		return "", fmt.Errorf("parse db connection string: %w", err)
	}
	if parsed.Database == "" {
		return "", fmt.Errorf("db connection string must include dbname")
	}

	return parsed.ConnString(), nil
}

func resolveSchema(schema string, id string) (string, error) {
	if schema == "" {
		return normalizeSchema("coreserver_" + dbSafeID(id))
	}

	if strings.Contains(schema, "{id}") {
		schema = strings.ReplaceAll(schema, "{id}", dbSafeID(id))
	}

	return normalizeSchema(schema)
}

func normalizeSchema(schema string) (string, error) {
	schema = strings.TrimSpace(schema)
	if schema == "" {
		return "", fmt.Errorf("db schema must not be empty")
	}
	if first := rune(schema[0]); !(unicode.IsLetter(first) || first == '_') {
		return "", fmt.Errorf("invalid db schema %q: first character must be a letter or '_'", schema)
	}

	for _, r := range schema {
		if unicode.IsLetter(r) || unicode.IsDigit(r) || r == '_' {
			continue
		}
		return "", fmt.Errorf("invalid db schema %q: only letters, digits and '_' are allowed", schema)
	}

	return schema, nil
}

func dbSafeID(id string) string {
	return strings.ReplaceAll(id, "-", "_")
}
