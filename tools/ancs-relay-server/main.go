package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, nil))
	store, err := OpenEventStore(envOrDefault("ANCS_DATA_FILE", "data/ancs-events.ndjson"))
	if err != nil {
		logger.Error("open event store", "error", err)
		os.Exit(1)
	}
	defer store.Close()

	server := &http.Server{
		Addr:              envOrDefault("ANCS_LISTEN_ADDR", ":8080"),
		Handler:           NewAPI(store, logger).Routes(),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      10 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		shutdownContext, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if err := server.Shutdown(shutdownContext); err != nil {
			logger.Error("shutdown HTTP server", "error", err)
		}
	}()

	certFile := os.Getenv("ANCS_TLS_CERT_FILE")
	keyFile := os.Getenv("ANCS_TLS_KEY_FILE")
	if (certFile == "") != (keyFile == "") {
		logger.Error("ANCS_TLS_CERT_FILE and ANCS_TLS_KEY_FILE must be configured together")
		os.Exit(1)
	}
	logger.Info("ANCS relay server listening",
		"address", server.Addr,
		"tls", certFile != "",
		"events", store.Count(),
	)
	if certFile != "" {
		err = server.ListenAndServeTLS(certFile, keyFile)
	} else {
		err = server.ListenAndServe()
	}
	if err != nil && !errors.Is(err, http.ErrServerClosed) {
		logger.Error("serve HTTP", "error", err)
		os.Exit(1)
	}
}

func envOrDefault(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}
