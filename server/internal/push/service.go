package push

import (
	"bytes"
	"context"
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"
)

const firebaseMessagingScope = "https://www.googleapis.com/auth/firebase.messaging"

type Config struct {
	ProjectID          string
	APIKey             string
	AppID              string
	ServiceAccountFile string
	RequestTimeout     time.Duration
}

type Service struct {
	cfg              Config
	logger           *slog.Logger
	httpClient       *http.Client
	clientEmail      string
	tokenURI         string
	privateKey       *rsa.PrivateKey
	mu               sync.Mutex
	cachedToken      string
	cachedTokenUntil time.Time
}

type SendError struct {
	StatusCode int
	Status     string
	Code       string
	Message    string
}

type sendRequest struct {
	Message sendMessage `json:"message"`
}

type sendMessage struct {
	Token        string            `json:"token"`
	Notification *notification     `json:"notification,omitempty"`
	Data         map[string]string `json:"data,omitempty"`
	Android      androidConfig     `json:"android"`
	APNS         apnsConfig        `json:"apns"`
}

type notification struct {
	Title string `json:"title"`
	Body  string `json:"body"`
}

type androidConfig struct {
	Priority string `json:"priority"`
}

type apnsConfig struct {
	Headers map[string]string `json:"headers,omitempty"`
	Payload apnsPayload       `json:"payload"`
}

type apnsPayload struct {
	APS apsPayload `json:"aps"`
}

type apsPayload struct {
	Sound            string `json:"sound,omitempty"`
	ContentAvailable int    `json:"content-available,omitempty"`
}

type fcmErrorResponse struct {
	Error fcmErrorBody `json:"error"`
}

type fcmErrorBody struct {
	Code    int              `json:"code"`
	Message string           `json:"message"`
	Status  string           `json:"status"`
	Details []fcmErrorDetail `json:"details"`
}

type fcmErrorDetail struct {
	Type      string `json:"@type"`
	ErrorCode string `json:"errorCode"`
}

type serviceAccountCredentials struct {
	ProjectID   string `json:"project_id"`
	PrivateKey  string `json:"private_key"`
	ClientEmail string `json:"client_email"`
	TokenURI    string `json:"token_uri"`
}

type tokenResponse struct {
	AccessToken string `json:"access_token"`
	ExpiresIn   int    `json:"expires_in"`
	TokenType   string `json:"token_type"`
}

func New(cfg Config, logger *slog.Logger) (*Service, error) {
	if strings.TrimSpace(cfg.ProjectID) == "" || strings.TrimSpace(cfg.ServiceAccountFile) == "" {
		logger.Info("fcm disabled: incomplete configuration",
			slog.Bool("has_project_id", strings.TrimSpace(cfg.ProjectID) != ""),
			slog.Bool("has_service_account_file", strings.TrimSpace(cfg.ServiceAccountFile) != ""))
		return nil, nil
	}
	if cfg.RequestTimeout <= 0 {
		cfg.RequestTimeout = 10 * time.Second
	}

	logger.Info("initializing fcm service",
		slog.String("fcm_project_id", cfg.ProjectID),
		slog.String("fcm_app_id", cfg.AppID),
		slog.String("service_account_file", cfg.ServiceAccountFile),
		slog.Duration("request_timeout", cfg.RequestTimeout))

	credentialsJSON, err := os.ReadFile(cfg.ServiceAccountFile)
	if err != nil {
		return nil, fmt.Errorf("read service account file: %w", err)
	}

	var credentials serviceAccountCredentials
	if err := json.Unmarshal(credentialsJSON, &credentials); err != nil {
		return nil, fmt.Errorf("parse service account file: %w", err)
	}
	if strings.TrimSpace(credentials.ClientEmail) == "" {
		return nil, errors.New("service account client_email is empty")
	}
	if strings.TrimSpace(credentials.PrivateKey) == "" {
		return nil, errors.New("service account private_key is empty")
	}
	privateKey, err := parseRSAPrivateKey(credentials.PrivateKey)
	if err != nil {
		return nil, fmt.Errorf("parse service account private key: %w", err)
	}

	tokenURI := strings.TrimSpace(credentials.TokenURI)
	if tokenURI == "" {
		tokenURI = "https://oauth2.googleapis.com/token"
	}

	service := &Service{
		cfg:         cfg,
		logger:      logger.With(slog.String("component", "fcm"), slog.String("fcm_project_id", cfg.ProjectID), slog.String("fcm_app_id", cfg.AppID)),
		httpClient:  &http.Client{Timeout: cfg.RequestTimeout},
		clientEmail: credentials.ClientEmail,
		tokenURI:    tokenURI,
		privateKey:  privateKey,
	}

	service.logger.Info("fcm service initialized",
		slog.String("token_uri", tokenURI),
		slog.String("client_email", credentials.ClientEmail))

	return service, nil
}

// NotifyNewMessage sends a standard "new message" push. It carries a visible
// notification payload, so a backgrounded app shows it in the tray without being
// woken.
func (s *Service) NotifyNewMessage(ctx context.Context, token string) error {
	if s == nil {
		return nil
	}

	return s.send(ctx, token, "new_message", sendMessage{
		Notification: &notification{
			Title: "New message",
			Body:  "You have received a new message.",
		},
		Data: map[string]string{
			"type":   "new_message",
			"app_id": s.cfg.AppID,
		},
		Android: androidConfig{Priority: "high"},
		APNS: apnsConfig{
			Headers: map[string]string{"apns-priority": "10"},
			Payload: apnsPayload{APS: apsPayload{Sound: "default"}},
		},
	})
}

// NotifyIncomingCall sends a data-only, high-priority push. It deliberately has
// no notification payload and no sensitive data — only type:"call" — so it wakes
// a killed/backgrounded app (via onMessageReceived) to come online and ring,
// while leaking nothing more than the fact that a call is being placed.
func (s *Service) NotifyIncomingCall(ctx context.Context, token string) error {
	if s == nil {
		return nil
	}

	return s.send(ctx, token, "call", sendMessage{
		Data: map[string]string{
			"type":   "call",
			"app_id": s.cfg.AppID,
		},
		Android: androidConfig{Priority: "high"},
		APNS: apnsConfig{
			// content-available wakes the iOS app in the background without a
			// user-visible alert; the app builds the call UI itself.
			Headers: map[string]string{"apns-priority": "10", "apns-push-type": "background"},
			Payload: apnsPayload{APS: apsPayload{ContentAvailable: 1}},
		},
	})
}

// NotifyCallCancelled sends a data-only push telling the recipient that an
// in-flight call was cancelled by the caller, so the app/notification stops
// ringing. Carries no sensitive data — only type:"call_cancel".
func (s *Service) NotifyCallCancelled(ctx context.Context, token string) error {
	if s == nil {
		return nil
	}

	return s.send(ctx, token, "call_cancel", sendMessage{
		Data: map[string]string{
			"type":   "call_cancel",
			"app_id": s.cfg.AppID,
		},
		Android: androidConfig{Priority: "high"},
		APNS: apnsConfig{
			Headers: map[string]string{"apns-priority": "10", "apns-push-type": "background"},
			Payload: apnsPayload{APS: apsPayload{ContentAvailable: 1}},
		},
	})
}

// send finalises the message (token + endpoint), authenticates and POSTs it to
// FCM, mapping a rejection to a *SendError so the caller can prune dead tokens.
func (s *Service) send(ctx context.Context, token, kind string, msg sendMessage) error {
	token = strings.TrimSpace(token)
	if token == "" {
		return errors.New("empty fcm token")
	}

	fingerprint := tokenFingerprint(token)
	s.logger.Info("sending fcm push",
		slog.String("kind", kind),
		slog.Int("token_length", len(token)),
		slog.String("token_fingerprint", fingerprint))

	accessToken, err := s.getAccessToken(ctx)
	if err != nil {
		s.logger.Error("failed to get fcm access token for push",
			slog.Any("error", err),
			slog.String("token_fingerprint", fingerprint))
		return fmt.Errorf("get access token: %w", err)
	}

	msg.Token = token
	payload, err := json.Marshal(sendRequest{Message: msg})
	if err != nil {
		return fmt.Errorf("marshal fcm request: %w", err)
	}

	endpoint := fmt.Sprintf("https://fcm.googleapis.com/v1/projects/%s/messages:send", url.PathEscape(s.cfg.ProjectID))
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint, bytes.NewReader(payload))
	if err != nil {
		return fmt.Errorf("build fcm request: %w", err)
	}
	req.Header.Set("Authorization", "Bearer "+accessToken)
	req.Header.Set("Content-Type", "application/json; charset=utf-8")

	resp, err := s.httpClient.Do(req)
	if err != nil {
		s.logger.Error("fcm request failed",
			slog.Any("error", err),
			slog.String("token_fingerprint", fingerprint))
		return fmt.Errorf("send fcm request: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode >= http.StatusOK && resp.StatusCode < http.StatusMultipleChoices {
		_, _ = io.Copy(io.Discard, resp.Body)
		s.logger.Info("fcm push sent",
			slog.String("kind", kind),
			slog.Int("status_code", resp.StatusCode),
			slog.String("token_fingerprint", fingerprint))
		return nil
	}

	body, readErr := io.ReadAll(io.LimitReader(resp.Body, 64*1024))
	if readErr != nil {
		return fmt.Errorf("read fcm error response: %w", readErr)
	}

	sendErr := &SendError{StatusCode: resp.StatusCode, Message: strings.TrimSpace(string(body))}
	var parsed fcmErrorResponse
	if err := json.Unmarshal(body, &parsed); err == nil {
		sendErr.Status = parsed.Error.Status
		sendErr.Message = parsed.Error.Message
		for _, detail := range parsed.Error.Details {
			if detail.ErrorCode != "" {
				sendErr.Code = detail.ErrorCode
				break
			}
		}
	}

	s.logger.Warn("fcm push rejected",
		slog.Int("status_code", sendErr.StatusCode),
		slog.String("status", sendErr.Status),
		slog.String("code", sendErr.Code),
		slog.String("message", sendErr.Message),
		slog.String("token_fingerprint", fingerprint))

	return sendErr
}

func (e *SendError) Error() string {
	parts := make([]string, 0, 3)
	if e.StatusCode > 0 {
		parts = append(parts, fmt.Sprintf("status_code=%d", e.StatusCode))
	}
	if e.Status != "" {
		parts = append(parts, "status="+e.Status)
	}
	if e.Code != "" {
		parts = append(parts, "code="+e.Code)
	}
	if e.Message != "" {
		parts = append(parts, e.Message)
	}
	if len(parts) == 0 {
		return "fcm send failed"
	}
	return "fcm send failed: " + strings.Join(parts, ", ")
}

func (e *SendError) ShouldDeleteToken() bool {
	return e != nil && (e.Code == "UNREGISTERED" || e.Code == "SENDER_ID_MISMATCH")
}

func (s *Service) getAccessToken(ctx context.Context) (string, error) {
	s.mu.Lock()
	if s.cachedToken != "" && time.Until(s.cachedTokenUntil) > 30*time.Second {
		token := s.cachedToken
		expiresAt := s.cachedTokenUntil
		s.mu.Unlock()
		s.logger.Debug("using cached fcm access token", slog.Time("expires_at", expiresAt))
		return token, nil
	}
	s.mu.Unlock()

	token, expiresAt, err := s.fetchAccessToken(ctx)
	if err != nil {
		return "", err
	}

	s.mu.Lock()
	s.cachedToken = token
	s.cachedTokenUntil = expiresAt
	s.mu.Unlock()

	s.logger.Info("refreshed fcm access token", slog.Time("expires_at", expiresAt))
	return token, nil
}

func (s *Service) fetchAccessToken(ctx context.Context) (string, time.Time, error) {
	s.logger.Info("requesting new fcm access token", slog.String("token_uri", s.tokenURI))

	assertion, err := s.buildJWT(time.Now())
	if err != nil {
		s.logger.Error("failed to build fcm jwt assertion", slog.Any("error", err))
		return "", time.Time{}, fmt.Errorf("build jwt assertion: %w", err)
	}

	form := url.Values{}
	form.Set("grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer")
	form.Set("assertion", assertion)

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.tokenURI, strings.NewReader(form.Encode()))
	if err != nil {
		s.logger.Error("failed to build fcm token request", slog.Any("error", err))
		return "", time.Time{}, fmt.Errorf("build token request: %w", err)
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")

	resp, err := s.httpClient.Do(req)
	if err != nil {
		s.logger.Error("failed to request fcm access token", slog.Any("error", err))
		return "", time.Time{}, fmt.Errorf("send token request: %w", err)
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, 64*1024))
	if err != nil {
		s.logger.Error("failed to read fcm token response", slog.Any("error", err))
		return "", time.Time{}, fmt.Errorf("read token response: %w", err)
	}
	if resp.StatusCode < http.StatusOK || resp.StatusCode >= http.StatusMultipleChoices {
		s.logger.Error("fcm token endpoint rejected request",
			slog.Int("status_code", resp.StatusCode),
			slog.String("body", strings.TrimSpace(string(body))))
		return "", time.Time{}, fmt.Errorf("token endpoint returned %d: %s", resp.StatusCode, strings.TrimSpace(string(body)))
	}

	var tokenResp tokenResponse
	if err := json.Unmarshal(body, &tokenResp); err != nil {
		s.logger.Error("failed to decode fcm token response", slog.Any("error", err))
		return "", time.Time{}, fmt.Errorf("decode token response: %w", err)
	}
	if tokenResp.AccessToken == "" {
		s.logger.Error("fcm token endpoint returned empty access token")
		return "", time.Time{}, errors.New("token endpoint returned empty access_token")
	}

	expiresIn := tokenResp.ExpiresIn
	if expiresIn <= 0 {
		expiresIn = 3600
	}

	expiresAt := time.Now().Add(time.Duration(expiresIn) * time.Second)
	s.logger.Info("received fcm access token",
		slog.Time("expires_at", expiresAt),
		slog.Int("expires_in_seconds", expiresIn))

	return tokenResp.AccessToken, expiresAt, nil
}

func tokenFingerprint(token string) string {
	sum := sha256.Sum256([]byte(token))
	return base64.RawURLEncoding.EncodeToString(sum[:6])
}

func (s *Service) buildJWT(now time.Time) (string, error) {
	header, err := json.Marshal(map[string]string{
		"alg": "RS256",
		"typ": "JWT",
	})
	if err != nil {
		return "", fmt.Errorf("marshal jwt header: %w", err)
	}

	claims, err := json.Marshal(map[string]any{
		"iss":   s.clientEmail,
		"scope": firebaseMessagingScope,
		"aud":   s.tokenURI,
		"iat":   now.Unix(),
		"exp":   now.Add(time.Hour).Unix(),
	})
	if err != nil {
		return "", fmt.Errorf("marshal jwt claims: %w", err)
	}

	encodedHeader := base64.RawURLEncoding.EncodeToString(header)
	encodedClaims := base64.RawURLEncoding.EncodeToString(claims)
	unsigned := encodedHeader + "." + encodedClaims

	digest := sha256.Sum256([]byte(unsigned))
	signature, err := rsa.SignPKCS1v15(rand.Reader, s.privateKey, crypto.SHA256, digest[:])
	if err != nil {
		return "", fmt.Errorf("sign jwt: %w", err)
	}

	return unsigned + "." + base64.RawURLEncoding.EncodeToString(signature), nil
}

func parseRSAPrivateKey(raw string) (*rsa.PrivateKey, error) {
	block, _ := pem.Decode([]byte(raw))
	if block == nil {
		return nil, errors.New("invalid PEM private key")
	}

	if privateKey, err := x509.ParsePKCS1PrivateKey(block.Bytes); err == nil {
		return privateKey, nil
	}

	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, err
	}

	privateKey, ok := parsed.(*rsa.PrivateKey)
	if !ok {
		return nil, errors.New("private key is not RSA")
	}

	return privateKey, nil
}
