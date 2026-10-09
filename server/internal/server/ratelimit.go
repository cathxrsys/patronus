package server

import (
	"math"
	"sync"
	"time"
)

// rateLimiter is a minimal token bucket. A nil *rateLimiter always allows, so
// callers can represent "disabled" without extra branching.
type rateLimiter struct {
	mu           sync.Mutex
	tokens       float64
	maxTokens    float64
	refillPerSec float64
	last         time.Time
}

// newRateLimiter returns a bucket that permits perMinute events per minute
// (bursting up to perMinute). perMinute <= 0 returns nil, i.e. unlimited.
func newRateLimiter(perMinute int) *rateLimiter {
	if perMinute <= 0 {
		return nil
	}
	return &rateLimiter{
		tokens:       float64(perMinute),
		maxTokens:    float64(perMinute),
		refillPerSec: float64(perMinute) / 60.0,
		last:         time.Now(),
	}
}

// allow consumes one token, refilling based on elapsed time. Returns false when
// the bucket is empty (caller is over the limit).
func (rl *rateLimiter) allow() bool {
	if rl == nil {
		return true
	}
	rl.mu.Lock()
	defer rl.mu.Unlock()

	now := time.Now()
	rl.tokens = math.Min(rl.maxTokens, rl.tokens+now.Sub(rl.last).Seconds()*rl.refillPerSec)
	rl.last = now

	if rl.tokens >= 1 {
		rl.tokens--
		return true
	}
	return false
}

// allowPrekeyDispense reports whether a prekey may be handed out for the given
// target identity right now. The limit is per target identity (not per
// requester) so it protects a member's prekey pool from being drained no matter
// who is asking. A non-positive configured rate disables it entirely.
func (s *Server) allowPrekeyDispense(targetHex string) bool {
	if s.prekeyRatePerMin <= 0 {
		return true
	}

	s.prekeyLimiterMu.Lock()
	rl, ok := s.prekeyLimiters[targetHex]
	if !ok {
		rl = newRateLimiter(s.prekeyRatePerMin)
		s.prekeyLimiters[targetHex] = rl
	}
	s.prekeyLimiterMu.Unlock()

	return rl.allow()
}
