package main

import (
	"fmt"
	"net"
	"regexp"
)

const (
	schemaVersion = 1
	eventType     = "ios_ancs_notification"
	boardName     = "lichuang-dev-ancs"
)

var (
	uuidPattern    = regexp.MustCompile(`^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$`)
	sessionPattern = regexp.MustCompile(`^[0-9a-f]{16}$`)
	categoryNames  = []string{
		"other",
		"incoming_call",
		"missed_call",
		"voicemail",
		"social",
		"schedule",
		"email",
		"news",
		"health_and_fitness",
		"business_and_finance",
		"location",
		"entertainment",
	}
)

type NotificationEnvelope struct {
	SchemaVersion int              `json:"schema_version"`
	Type          string           `json:"type"`
	EventID       string           `json:"event_id"`
	Device        DeviceIdentity   `json:"device"`
	ANCS          ANCSNotification `json:"ancs"`
}

type DeviceIdentity struct {
	ID    string `json:"id"`
	MAC   string `json:"mac"`
	Board string `json:"board"`
}

type ANCSNotification struct {
	SessionID     string            `json:"session_id"`
	Sequence      uint64            `json:"sequence"`
	Event         string            `json:"event"`
	UID           uint32            `json:"uid"`
	CategoryID    uint8             `json:"category_id"`
	Category      string            `json:"category"`
	EventFlags    uint8             `json:"event_flags"`
	Flags         NotificationFlags `json:"flags"`
	AppIdentifier string            `json:"app_identifier"`
	Title         string            `json:"title"`
	Subtitle      string            `json:"subtitle"`
	Message       string            `json:"message"`
	Truncated     bool              `json:"truncated"`
}

type NotificationFlags struct {
	Silent         bool `json:"silent"`
	Important      bool `json:"important"`
	PreExisting    bool `json:"pre_existing"`
	PositiveAction bool `json:"positive_action"`
	NegativeAction bool `json:"negative_action"`
}

func (event NotificationEnvelope) Validate(deviceIDHeader, clientIDHeader string) error {
	if event.SchemaVersion != schemaVersion {
		return fmt.Errorf("schema_version must be %d", schemaVersion)
	}
	if event.Type != eventType {
		return fmt.Errorf("type must be %q", eventType)
	}
	if !uuidPattern.MatchString(event.Device.ID) {
		return fmt.Errorf("device.id must be a UUID")
	}
	if _, err := net.ParseMAC(event.Device.MAC); err != nil {
		return fmt.Errorf("device.mac must be a MAC address")
	}
	if event.Device.Board != boardName {
		return fmt.Errorf("device.board must be %q", boardName)
	}
	if deviceIDHeader != event.Device.MAC {
		return fmt.Errorf("Device-Id header does not match device.mac")
	}
	if clientIDHeader != event.Device.ID {
		return fmt.Errorf("Client-Id header does not match device.id")
	}
	if !sessionPattern.MatchString(event.ANCS.SessionID) {
		return fmt.Errorf("ancs.session_id must contain 16 lowercase hex characters")
	}
	if event.ANCS.Sequence == 0 {
		return fmt.Errorf("ancs.sequence must be greater than zero")
	}
	expectedEventID := fmt.Sprintf("%s:%s:%d", event.Device.ID, event.ANCS.SessionID, event.ANCS.Sequence)
	if event.EventID != expectedEventID {
		return fmt.Errorf("event_id does not match device, session, and sequence")
	}
	switch event.ANCS.Event {
	case "added", "modified", "removed":
	default:
		return fmt.Errorf("ancs.event must be added, modified, or removed")
	}
	expectedCategory := "unknown"
	if int(event.ANCS.CategoryID) < len(categoryNames) {
		expectedCategory = categoryNames[event.ANCS.CategoryID]
	}
	if event.ANCS.Category != expectedCategory {
		return fmt.Errorf("ancs.category does not match category_id")
	}
	if event.ANCS.Flags.Silent != flagSet(event.ANCS.EventFlags, 0) ||
		event.ANCS.Flags.Important != flagSet(event.ANCS.EventFlags, 1) ||
		event.ANCS.Flags.PreExisting != flagSet(event.ANCS.EventFlags, 2) ||
		event.ANCS.Flags.PositiveAction != flagSet(event.ANCS.EventFlags, 3) ||
		event.ANCS.Flags.NegativeAction != flagSet(event.ANCS.EventFlags, 4) {
		return fmt.Errorf("ancs.flags does not match event_flags")
	}
	if len(event.ANCS.AppIdentifier) > 512 {
		return fmt.Errorf("ancs.app_identifier exceeds 512 bytes")
	}
	if len(event.ANCS.Title) > 256 {
		return fmt.Errorf("ancs.title exceeds 256 bytes")
	}
	if len(event.ANCS.Subtitle) > 256 {
		return fmt.Errorf("ancs.subtitle exceeds 256 bytes")
	}
	if len(event.ANCS.Message) > 1024 {
		return fmt.Errorf("ancs.message exceeds 1024 bytes")
	}
	if len(event.ANCS.AppIdentifier)+len(event.ANCS.Title)+len(event.ANCS.Subtitle)+len(event.ANCS.Message) > 2048 {
		return fmt.Errorf("ANCS text exceeds 2048 bytes in total")
	}
	return nil
}

func flagSet(flags uint8, bit uint8) bool {
	return flags&(1<<bit) != 0
}
