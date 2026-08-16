package main

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
)

const (
	testDeviceID = "123e4567-e89b-42d3-a456-426614174000"
	testMAC      = "aa:bb:cc:dd:ee:ff"
	testSession  = "0123456789abcdef"
)

func TestReceiveAndDeduplicateNotification(t *testing.T) {
	store, handler := newTestAPI(t)
	event := validEvent()

	response := sendEvent(t, handler, event)
	if response.Code != http.StatusAccepted {
		t.Fatalf("first request status = %d, body = %s", response.Code, response.Body.String())
	}
	response = sendEvent(t, handler, event)
	if response.Code != http.StatusOK {
		t.Fatalf("duplicate request status = %d, body = %s", response.Code, response.Body.String())
	}
	if store.Count() != 1 {
		t.Fatalf("stored event count = %d, want 1", store.Count())
	}
}

func TestPersistenceRestoresIdempotency(t *testing.T) {
	path := filepath.Join(t.TempDir(), "events.ndjson")
	store, err := OpenEventStore(path)
	if err != nil {
		t.Fatal(err)
	}
	if stored, err := store.Save(validEvent()); err != nil || !stored {
		t.Fatalf("Save() = %v, %v", stored, err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	reopened, err := OpenEventStore(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { reopened.Close() })
	if stored, err := reopened.Save(validEvent()); err != nil || stored {
		t.Fatalf("duplicate Save() = %v, %v", stored, err)
	}
}

func TestRejectsInvalidRequests(t *testing.T) {
	_, handler := newTestAPI(t)
	tests := []struct {
		name   string
		mutate func(*NotificationEnvelope)
	}{
		{name: "event id", mutate: func(event *NotificationEnvelope) { event.EventID = "wrong" }},
		{name: "board", mutate: func(event *NotificationEnvelope) { event.Device.Board = "other" }},
		{name: "flags", mutate: func(event *NotificationEnvelope) { event.ANCS.Flags.Important = false }},
		{name: "category", mutate: func(event *NotificationEnvelope) { event.ANCS.Category = "email" }},
		{name: "message limit", mutate: func(event *NotificationEnvelope) { event.ANCS.Message = strings.Repeat("x", 1025) }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			event := validEvent()
			test.mutate(&event)
			response := sendEvent(t, handler, event)
			if response.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, body = %s", response.Code, response.Body.String())
			}
		})
	}
}

func TestRejectsMissingAndUnknownFields(t *testing.T) {
	_, handler := newTestAPI(t)
	event := validEvent()
	body, err := json.Marshal(event)
	if err != nil {
		t.Fatal(err)
	}

	var object map[string]any
	if err := json.Unmarshal(body, &object); err != nil {
		t.Fatal(err)
	}
	delete(object, "event_id")
	response := sendRaw(t, handler, object, "application/json")
	if response.Code != http.StatusBadRequest {
		t.Fatalf("missing field status = %d", response.Code)
	}

	object["event_id"] = event.EventID
	object["unexpected"] = true
	response = sendRaw(t, handler, object, "application/json")
	if response.Code != http.StatusBadRequest {
		t.Fatalf("unknown field status = %d", response.Code)
	}

	response = sendRaw(t, handler, event, "text/plain")
	if response.Code != http.StatusUnsupportedMediaType {
		t.Fatalf("content type status = %d", response.Code)
	}
}

func TestHealthAndMethodHandling(t *testing.T) {
	_, handler := newTestAPI(t)
	request := httptest.NewRequest(http.MethodGet, "/healthz", nil)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("health status = %d", response.Code)
	}

	request = httptest.NewRequest(http.MethodGet, "/api/v1/ancs/notifications", nil)
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusMethodNotAllowed {
		t.Fatalf("method status = %d", response.Code)
	}
}

func newTestAPI(t *testing.T) (*EventStore, http.Handler) {
	t.Helper()
	store, err := OpenEventStore(filepath.Join(t.TempDir(), "events.ndjson"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { store.Close() })
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	return store, NewAPI(store, logger).Routes()
}

func validEvent() NotificationEnvelope {
	return NotificationEnvelope{
		SchemaVersion: schemaVersion,
		Type:          eventType,
		EventID:       testDeviceID + ":" + testSession + ":1",
		Device: DeviceIdentity{
			ID:    testDeviceID,
			MAC:   testMAC,
			Board: boardName,
		},
		ANCS: ANCSNotification{
			SessionID:     testSession,
			Sequence:      1,
			Event:         "added",
			UID:           123,
			CategoryID:    4,
			Category:      "social",
			EventFlags:    2,
			Flags:         NotificationFlags{Important: true},
			AppIdentifier: "com.example.app",
			Title:         "Title",
			Message:       "Body",
		},
	}
}

func sendEvent(t *testing.T, handler http.Handler, event NotificationEnvelope) *httptest.ResponseRecorder {
	t.Helper()
	return sendRaw(t, handler, event, "application/json")
}

func sendRaw(t *testing.T, handler http.Handler, body any, contentType string) *httptest.ResponseRecorder {
	t.Helper()
	encoded, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodPost, "/api/v1/ancs/notifications", bytes.NewReader(encoded))
	request.Header.Set("Content-Type", contentType)
	request.Header.Set("Device-Id", testMAC)
	request.Header.Set("Client-Id", testDeviceID)
	request.Header.Set("User-Agent", "xiaozhi-esp32-ancs-relay/1")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	return response
}
