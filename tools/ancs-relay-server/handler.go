package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"mime"
	"net/http"
)

const maxRequestBytes = 64 * 1024

type API struct {
	store  *EventStore
	logger *slog.Logger
}

func NewAPI(store *EventStore, logger *slog.Logger) *API {
	return &API{store: store, logger: logger}
}

func (api *API) Routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", api.health)
	mux.HandleFunc("/api/v1/ancs/notifications", api.receiveNotification)
	return mux
}

func (api *API) health(writer http.ResponseWriter, request *http.Request) {
	if request.Method != http.MethodGet {
		writer.Header().Set("Allow", http.MethodGet)
		writeError(writer, http.StatusMethodNotAllowed, "method_not_allowed", "use GET")
		return
	}
	count, err := api.store.Count(request.Context())
	if err != nil {
		writeError(writer, http.StatusServiceUnavailable, "storage_unavailable", "SQLite is unavailable")
		return
	}
	writeJSON(writer, http.StatusOK, map[string]any{
		"status": "ok",
		"events": count,
	})
}

func (api *API) receiveNotification(writer http.ResponseWriter, request *http.Request) {
	if request.Method != http.MethodPost {
		writer.Header().Set("Allow", http.MethodPost)
		writeError(writer, http.StatusMethodNotAllowed, "method_not_allowed", "use POST")
		return
	}
	mediaType, _, err := mime.ParseMediaType(request.Header.Get("Content-Type"))
	if err != nil || mediaType != "application/json" {
		writeError(writer, http.StatusUnsupportedMediaType, "unsupported_media_type", "Content-Type must be application/json")
		return
	}
	deviceID := request.Header.Get("Device-Id")
	clientID := request.Header.Get("Client-Id")
	if deviceID == "" || clientID == "" || request.Header.Get("User-Agent") == "" {
		writeError(writer, http.StatusBadRequest, "missing_headers", "Device-Id, Client-Id, and User-Agent are required")
		return
	}

	request.Body = http.MaxBytesReader(writer, request.Body, maxRequestBytes)
	body, err := io.ReadAll(request.Body)
	if err != nil {
		var tooLarge *http.MaxBytesError
		if errors.As(err, &tooLarge) {
			writeError(writer, http.StatusRequestEntityTooLarge, "body_too_large", "request body exceeds 64 KiB")
			return
		}
		writeError(writer, http.StatusBadRequest, "read_failed", "unable to read request body")
		return
	}
	event, err := decodeEnvelope(body)
	if err != nil {
		writeError(writer, http.StatusBadRequest, "invalid_json", err.Error())
		return
	}
	if err := event.Validate(deviceID, clientID); err != nil {
		writeError(writer, http.StatusBadRequest, "invalid_event", err.Error())
		return
	}

	stored, err := api.store.Save(request.Context(), event)
	if err != nil {
		api.logger.Error("store ANCS event", "event_id", event.EventID, "error", err)
		writeError(writer, http.StatusServiceUnavailable, "storage_unavailable", "event was not durably stored")
		return
	}
	if !stored {
		api.logger.Info("duplicate ANCS event", "event_id", event.EventID)
		writeJSON(writer, http.StatusOK, map[string]any{
			"status":   "duplicate",
			"event_id": event.EventID,
		})
		return
	}

	api.logger.Info("stored ANCS event",
		"event_id", event.EventID,
		"event", event.ANCS.Event,
		"app_identifier", event.ANCS.AppIdentifier,
		"title", event.ANCS.Title,
		"message", event.ANCS.Message,
	)
	writeJSON(writer, http.StatusAccepted, map[string]any{
		"status":   "accepted",
		"event_id": event.EventID,
	})
}

func decodeEnvelope(body []byte) (NotificationEnvelope, error) {
	var event NotificationEnvelope
	decoder := json.NewDecoder(bytes.NewReader(body))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&event); err != nil {
		return event, fmt.Errorf("decode JSON: %w", err)
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return event, errors.New("request body must contain exactly one JSON object")
	}
	if err := requireFields(body); err != nil {
		return event, err
	}
	return event, nil
}

func requireFields(body []byte) error {
	var root map[string]json.RawMessage
	if err := json.Unmarshal(body, &root); err != nil {
		return err
	}
	if err := requireObjectFields(root, "root", "schema_version", "type", "event_id", "device", "ancs"); err != nil {
		return err
	}
	var device map[string]json.RawMessage
	if err := json.Unmarshal(root["device"], &device); err != nil {
		return errors.New("device must be an object")
	}
	if err := requireObjectFields(device, "device", "id", "mac", "board"); err != nil {
		return err
	}
	var ancs map[string]json.RawMessage
	if err := json.Unmarshal(root["ancs"], &ancs); err != nil {
		return errors.New("ancs must be an object")
	}
	if err := requireObjectFields(ancs, "ancs", "session_id", "sequence", "event", "uid", "category_id", "category", "event_flags", "flags", "app_identifier", "title", "subtitle", "message", "truncated"); err != nil {
		return err
	}
	var flags map[string]json.RawMessage
	if err := json.Unmarshal(ancs["flags"], &flags); err != nil {
		return errors.New("ancs.flags must be an object")
	}
	return requireObjectFields(flags, "ancs.flags", "silent", "important", "pre_existing", "positive_action", "negative_action")
}

func requireObjectFields(object map[string]json.RawMessage, name string, fields ...string) error {
	for _, field := range fields {
		if _, exists := object[field]; !exists {
			return fmt.Errorf("%s.%s is required", name, field)
		}
	}
	return nil
}

func writeError(writer http.ResponseWriter, status int, code, message string) {
	writeJSON(writer, status, map[string]any{
		"error":   code,
		"message": message,
	})
}

func writeJSON(writer http.ResponseWriter, status int, body any) {
	writer.Header().Set("Content-Type", "application/json")
	writer.WriteHeader(status)
	_ = json.NewEncoder(writer).Encode(body)
}
