package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync"
)

type EventStore struct {
	mu   sync.Mutex
	file *os.File
	seen map[string]struct{}
}

func OpenEventStore(path string) (*EventStore, error) {
	if path == "" {
		return nil, errors.New("event data path is empty")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return nil, fmt.Errorf("create event data directory: %w", err)
	}
	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR|os.O_APPEND, 0o600)
	if err != nil {
		return nil, fmt.Errorf("open event data file: %w", err)
	}
	store := &EventStore{file: file, seen: make(map[string]struct{})}
	if err := store.load(); err != nil {
		file.Close()
		return nil, err
	}
	return store, nil
}

func (store *EventStore) load() error {
	if _, err := store.file.Seek(0, 0); err != nil {
		return fmt.Errorf("seek event data file: %w", err)
	}
	scanner := bufio.NewScanner(store.file)
	scanner.Buffer(make([]byte, 64*1024), 256*1024)
	line := 0
	for scanner.Scan() {
		line++
		var record struct {
			EventID string `json:"event_id"`
		}
		if err := json.Unmarshal(scanner.Bytes(), &record); err != nil || record.EventID == "" {
			return fmt.Errorf("invalid event data at line %d", line)
		}
		store.seen[record.EventID] = struct{}{}
	}
	if err := scanner.Err(); err != nil {
		return fmt.Errorf("scan event data file: %w", err)
	}
	_, err := store.file.Seek(0, 2)
	return err
}

func (store *EventStore) Save(event NotificationEnvelope) (bool, error) {
	store.mu.Lock()
	defer store.mu.Unlock()
	if _, exists := store.seen[event.EventID]; exists {
		return false, nil
	}
	encoded, err := json.Marshal(event)
	if err != nil {
		return false, fmt.Errorf("encode event: %w", err)
	}
	encoded = append(encoded, '\n')
	if _, err := store.file.Write(encoded); err != nil {
		return false, fmt.Errorf("append event: %w", err)
	}
	store.seen[event.EventID] = struct{}{}
	if err := store.file.Sync(); err != nil {
		return false, fmt.Errorf("sync event data: %w", err)
	}
	return true, nil
}

func (store *EventStore) Count() int {
	store.mu.Lock()
	defer store.mu.Unlock()
	return len(store.seen)
}

func (store *EventStore) Close() error {
	store.mu.Lock()
	defer store.mu.Unlock()
	return store.file.Close()
}
