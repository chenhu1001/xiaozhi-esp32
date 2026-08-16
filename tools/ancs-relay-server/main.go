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
	if len(os.Args) == 2 && os.Args[1] == "healthcheck" {
		runHealthcheck()
		return
	}
	logger := slog.New(slog.NewJSONHandler(os.Stdout, nil))
	store, err := OpenEventStore(envOrDefault("ANCS_DB_FILE", "data/ancs.db"))
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

	logger.Info("ANCS relay server listening",
		"address", server.Addr,
		"protocol", "http",
	)
	err = server.ListenAndServe()
	if err != nil && !errors.Is(err, http.ErrServerClosed) {
		logger.Error("serve HTTP", "error", err)
		os.Exit(1)
	}
}

func runHealthcheck() {
	client := &http.Client{Timeout: 3 * time.Second}
	response, err := client.Get("http://127.0.0.1:8080/healthz")
	if err != nil || response.StatusCode != http.StatusOK {
		os.Exit(1)
	}
	response.Body.Close()
}

func envOrDefault(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}
