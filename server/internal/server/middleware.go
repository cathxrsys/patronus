package server

import "net/http"

type Middleware func(http.Handler) http.Handler

func ChainMiddlewares(handler http.Handler, middlewares ...Middleware) http.Handler {
	for idx := len(middlewares) - 1; idx >= 0; idx-- {
		handler = middlewares[idx](handler)
	}
	return handler
}

func (s *Server) Use(middlewares ...Middleware) {
	s.mws = append(s.mws, middlewares...)
}

func (s *Server) withMiddlewares(handler http.Handler, middlewares ...Middleware) http.Handler {
	if len(s.mws) == 0 && len(middlewares) == 0 {
		return handler
	}

	chain := make([]Middleware, 0, len(s.mws)+len(middlewares))
	chain = append(chain, s.mws...)
	chain = append(chain, middlewares...)
	return ChainMiddlewares(handler, chain...)
}
