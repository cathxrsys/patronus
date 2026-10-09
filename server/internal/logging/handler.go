package logging

import (
	"bytes"
	"io"
	"log/slog"
	"os"
	"strings"
)

const (
	ansiReset  = "\033[0m"
	ansiBlue   = "\033[34m"
	ansiOrange = "\033[38;5;214m"
	ansiRed    = "\033[31m"
	ansiGray   = "\033[90m"
)

func NewTextHandler(w io.Writer, opts *slog.HandlerOptions) slog.Handler {
	if opts == nil {
		opts = &slog.HandlerOptions{}
	}
	if shouldColorize(w) {
		w = &colorizingWriter{next: w}
	}

	return slog.NewTextHandler(w, opts)
}

func shouldColorize(w io.Writer) bool {
	if os.Getenv("NO_COLOR") != "" {
		return false
	}
	if strings.EqualFold(os.Getenv("TERM"), "dumb") {
		return false
	}

	file, ok := w.(*os.File)
	if !ok {
		return false
	}

	info, err := file.Stat()
	if err != nil {
		return false
	}

	return info.Mode()&os.ModeCharDevice != 0
}

func colorizeLevel(level string) string {
	upperLevel := strings.ToUpper(level)

	switch upperLevel {
	case "INFO":
		return ansiBlue + upperLevel + ansiReset
	case "WARN":
		return ansiOrange + upperLevel + ansiReset
	case "ERROR":
		return ansiRed + upperLevel + ansiReset
	case "DEBUG":
		return ansiGray + upperLevel + ansiReset
	default:
		return upperLevel
	}
}

type colorizingWriter struct {
	next io.Writer
}

func (w *colorizingWriter) Write(p []byte) (int, error) {
	colored := colorizeLine(p)
	_, err := w.next.Write(colored)
	if err != nil {
		return 0, err
	}
	return len(p), nil
}

func colorizeLine(line []byte) []byte {
	replacements := []struct {
		plain   []byte
		colored []byte
	}{
		{plain: []byte("level=DEBUG"), colored: []byte("level=" + colorizeLevel("DEBUG"))},
		{plain: []byte("level=INFO"), colored: []byte("level=" + colorizeLevel("INFO"))},
		{plain: []byte("level=WARN"), colored: []byte("level=" + colorizeLevel("WARN"))},
		{plain: []byte("level=ERROR"), colored: []byte("level=" + colorizeLevel("ERROR"))},
	}

	result := line
	for _, replacement := range replacements {
		result = bytes.ReplaceAll(result, replacement.plain, replacement.colored)
	}

	return result
}
