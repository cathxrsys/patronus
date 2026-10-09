package server

import (
	"context"
	"net/http"
	"strings"
)

type sessionPrincipal struct {
	IdentityHex string
}

type sessionPrincipalContextKey struct{}

func (s *Server) requireSessionToken(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		token := extractBearerToken(r.Header.Get("Authorization"))
		if token == "" {
			http.NotFound(w, r)
			return
		}

		session := s.sessions.FindByToken(token)
		if session == nil || session.identityHex == "" {
			http.NotFound(w, r)
			return
		}

		// File and FCM-config endpoints are member-only. A provisional
		// (non-whitelisted) identity may hold a valid session token but must not
		// reach these, so it cannot use the server as file storage. Unlike the
		// missing/invalid-token cases above, the caller here already holds a
		// valid session, so returning 403 (rather than 404) does not leak the
		// endpoint's existence to an unauthenticated prober - it just tells an
		// already-known identity why the request was refused.
		if session.provisional.Load() {
			w.WriteHeader(http.StatusForbidden)
			return
		}

		principal := sessionPrincipal{IdentityHex: session.identityHex}
		ctx := context.WithValue(r.Context(), sessionPrincipalContextKey{}, principal)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

func principalFromContext(ctx context.Context) (sessionPrincipal, bool) {
	principal, ok := ctx.Value(sessionPrincipalContextKey{}).(sessionPrincipal)
	return principal, ok
}

func extractBearerToken(value string) string {
	scheme, token, ok := strings.Cut(strings.TrimSpace(value), " ")
	if !ok || !strings.EqualFold(scheme, "Bearer") {
		return ""
	}
	return strings.TrimSpace(token)
}
