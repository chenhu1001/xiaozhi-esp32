# Lichuang ESP32-S3 iOS ANCS Relay

The `lichuang-dev-ancs` variant receives iPhone notifications through Apple's
ANCS Bluetooth service and forwards each event to an HTTPS webhook. It does not
need or include an iOS companion app. The normal `lichuang-dev` variant does not
enable Bluetooth or notification forwarding. ANCS events are not displayed,
spoken, or sent through the existing WebSocket/MQTT protocol.

## Build

Use ESP-IDF 6.0.2 and the canonical board build command:

```sh
python scripts/build.py lckfb/szpi-esp32s3 --name lichuang-dev-ancs
```

Configure the device's Wi-Fi through the existing hotspot provisioning flow.
The ANCS variant intentionally disables BluFi because both features would own
the Bluetooth stack.

Before deploying, replace `kAncsRelayEndpoint` in
`main/bluetooth/ancs_relay_config.h`. Its checked-in value is deliberately
non-routable:

```text
https://example.invalid/api/v1/ancs/notifications
```

The endpoint must present a certificate accepted by the firmware HTTP client.
Version 1 does not send an authorization credential.

## Pair An iPhone

1. Flash and boot the `lichuang-dev-ancs` firmware.
2. On the iPhone, open Settings > Bluetooth.
3. Select `Xiaozhi ANCS` and accept the pairing request.
4. Allow the device to receive system notifications when iOS asks.
5. Leave Bluetooth enabled. The board advertises again after a disconnect and
   iOS can reconnect using its saved bond.

The firmware accepts one saved phone. To replace it, run `idf.py erase-flash`,
flash the firmware again, and remove the old `Xiaozhi ANCS` entry from the
iPhone if necessary. Erasing flash also removes Wi-Fi credentials. A normal
`idf.py flash` or OTA update does not remove the Bluetooth bond.

## Webhook Contract

Each Added, Modified, or Removed event is one `POST` with these headers:

```text
Content-Type: application/json
Device-Id: <Wi-Fi MAC address>
Client-Id: <persistent board UUID>
User-Agent: xiaozhi-esp32-ancs-relay/1
```

Example body:

```json
{
  "schema_version": 1,
  "type": "ios_ancs_notification",
  "event_id": "<device_uuid>:<ancs_session_id>:<sequence>",
  "device": {
    "id": "<board_uuid>",
    "mac": "<wifi_mac>",
    "board": "lichuang-dev-ancs"
  },
  "ancs": {
    "session_id": "<per-connection-random-id>",
    "sequence": 1,
    "event": "added",
    "uid": 123,
    "category_id": 4,
    "category": "social",
    "event_flags": 2,
    "flags": {
      "silent": false,
      "important": true,
      "pre_existing": false,
      "positive_action": false,
      "negative_action": false
    },
    "app_identifier": "com.example.app",
    "title": "Title",
    "subtitle": "",
    "message": "Body",
    "truncated": false
  }
}
```

Use `event_id` as the idempotency key. ANCS notification UIDs are valid only
inside one Bluetooth connection session. Any HTTP 2xx response acknowledges an
event. Network errors and HTTP 408, 429, and 5xx responses retry after
1/2/4/8/16/30 seconds; other HTTP statuses permanently discard the event.

The in-memory FIFO stores 20 events and drops the oldest event when full. It is
cleared by reboot and notification bodies are never persisted to flash.
UART logs always include the application identifier, title, message, retry, and
final forwarding result; queue-overflow logs identify events discarded before
delivery.

## Privacy And Limits

Notification application IDs, titles, and bodies are always printed to UART
and are sent without application-level authentication. Restrict physical serial
access, use a private HTTPS endpoint, and add authentication before production
deployment. ANCS does not guarantee that the service is always present or that
every application supplies complete text. If an attribute query fails or times
out, the event is still sent with the available metadata and
`"truncated": true`. Notifications marked `PreExisting` are intentionally
ignored.

## Hardware Acceptance

Verify the following on a real board and iPhone:

- first pairing from iOS Settings and the notification authorization prompt;
- Added, Modified, and Removed webhook bodies, including Chinese and Emoji;
- Wi-Fi loss with up to 20 queued events and forwarding after recovery;
- reboot reconnect using the persisted bond, and long-running reconnects;
- rejection of a second phone while the first bond exists;
- normal voice capture, playback, wake word, interruption, and server reconnect;
- webhook idempotency when a request succeeds server-side but its response is
  lost and the device retries.

Apple's protocol behavior is defined by the
[ANCS specification](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/Specification/Specification.html).
