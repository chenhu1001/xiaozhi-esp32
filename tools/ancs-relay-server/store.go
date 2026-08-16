package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"

	_ "modernc.org/sqlite"
)

type EventStore struct {
	db *sql.DB
}

func OpenEventStore(path string) (*EventStore, error) {
	if path == "" {
		return nil, errors.New("SQLite database path is empty")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return nil, fmt.Errorf("create SQLite directory: %w", err)
	}
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, fmt.Errorf("open SQLite database: %w", err)
	}
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	store := &EventStore{db: db}
	if err := store.initialize(context.Background()); err != nil {
		db.Close()
		return nil, err
	}
	return store, nil
}

func (store *EventStore) initialize(ctx context.Context) error {
	statements := []string{
		`PRAGMA journal_mode = WAL`,
		`PRAGMA synchronous = NORMAL`,
		`PRAGMA busy_timeout = 5000`,
		`CREATE TABLE IF NOT EXISTS notifications (
			event_id TEXT PRIMARY KEY,
			received_at TEXT NOT NULL,
			schema_version INTEGER NOT NULL,
			type TEXT NOT NULL,
			device_id TEXT NOT NULL,
			device_mac TEXT NOT NULL,
			board TEXT NOT NULL,
			session_id TEXT NOT NULL,
			sequence INTEGER NOT NULL,
			event TEXT NOT NULL,
			uid INTEGER NOT NULL,
			category_id INTEGER NOT NULL,
			category TEXT NOT NULL,
			event_flags INTEGER NOT NULL,
			silent INTEGER NOT NULL,
			important INTEGER NOT NULL,
			pre_existing INTEGER NOT NULL,
			positive_action INTEGER NOT NULL,
			negative_action INTEGER NOT NULL,
			app_identifier TEXT NOT NULL,
			title TEXT NOT NULL,
			subtitle TEXT NOT NULL,
			message TEXT NOT NULL,
			truncated INTEGER NOT NULL,
			raw_json TEXT NOT NULL
		)`,
		`CREATE INDEX IF NOT EXISTS idx_notifications_device_time
			ON notifications(device_id, received_at DESC)`,
		`CREATE INDEX IF NOT EXISTS idx_notifications_app_time
			ON notifications(app_identifier, received_at DESC)`,
		`CREATE INDEX IF NOT EXISTS idx_notifications_session_uid
			ON notifications(session_id, uid)`,
		`CREATE INDEX IF NOT EXISTS idx_notifications_event_time
			ON notifications(event, received_at DESC)`,
	}
	for _, statement := range statements {
		if _, err := store.db.ExecContext(ctx, statement); err != nil {
			return fmt.Errorf("initialize SQLite database: %w", err)
		}
	}
	return nil
}

func (store *EventStore) Save(ctx context.Context, event NotificationEnvelope) (bool, error) {
	rawJSON, err := json.Marshal(event)
	if err != nil {
		return false, fmt.Errorf("encode event: %w", err)
	}
	result, err := store.db.ExecContext(ctx, `
		INSERT OR IGNORE INTO notifications (
			event_id, received_at, schema_version, type,
			device_id, device_mac, board,
			session_id, sequence, event, uid, category_id, category, event_flags,
			silent, important, pre_existing, positive_action, negative_action,
			app_identifier, title, subtitle, message, truncated, raw_json
		) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		event.EventID,
		time.Now().UTC().Format(time.RFC3339Nano),
		event.SchemaVersion,
		event.Type,
		event.Device.ID,
		event.Device.MAC,
		event.Device.Board,
		event.ANCS.SessionID,
		int64(event.ANCS.Sequence),
		event.ANCS.Event,
		int64(event.ANCS.UID),
		int64(event.ANCS.CategoryID),
		event.ANCS.Category,
		int64(event.ANCS.EventFlags),
		event.ANCS.Flags.Silent,
		event.ANCS.Flags.Important,
		event.ANCS.Flags.PreExisting,
		event.ANCS.Flags.PositiveAction,
		event.ANCS.Flags.NegativeAction,
		event.ANCS.AppIdentifier,
		event.ANCS.Title,
		event.ANCS.Subtitle,
		event.ANCS.Message,
		event.ANCS.Truncated,
		string(rawJSON),
	)
	if err != nil {
		return false, fmt.Errorf("insert event into SQLite: %w", err)
	}
	rowsAffected, err := result.RowsAffected()
	if err != nil {
		return false, fmt.Errorf("read SQLite insert result: %w", err)
	}
	return rowsAffected == 1, nil
}

func (store *EventStore) Count(ctx context.Context) (int, error) {
	var count int
	if err := store.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM notifications`).Scan(&count); err != nil {
		return 0, fmt.Errorf("count SQLite events: %w", err)
	}
	return count, nil
}

func (store *EventStore) Close() error {
	return store.db.Close()
}
