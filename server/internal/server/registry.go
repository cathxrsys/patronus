package server

import "sync"

type Registry struct {
	mu       sync.RWMutex
	sessions map[string]*Session
	tokens   map[string]*Session
}

func NewRegistry() *Registry {
	return &Registry{
		sessions: make(map[string]*Session),
		tokens:   make(map[string]*Session),
	}
}

func (r *Registry) Add(identity string, session *Session) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.sessions[identity] = session
	if session.accessToken != "" {
		r.tokens[session.accessToken] = session
	}
}

func (r *Registry) Remove(identity string, session *Session) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if current, ok := r.sessions[identity]; ok && current == session {
		delete(r.sessions, identity)
	}
	if session.accessToken != "" {
		if current, ok := r.tokens[session.accessToken]; ok && current == session {
			delete(r.tokens, session.accessToken)
		}
	}
}

func (r *Registry) Find(identity string) *Session {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.sessions[identity]
}

func (r *Registry) FindByToken(token string) *Session {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.tokens[token]
}
