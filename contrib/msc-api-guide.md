# OsmoMSC Embedded HTTP API

Native JSON API inside `osmo-msc`. Configure under the `msc` VTY node:

```
msc
 api
  bind-ip 0.0.0.0
  port 8080
  token your-secret-token
!
```

Restart `osmo-msc` after changing API settings.

## Authentication

Every request must include the token:

```http
Authorization: Bearer <token>
```

Alternative:

```http
X-Api-Token: <token>
```

## Base URL

```
http://<msc-host>:<port>
```

Default port: `8080`.

---

## Subscribers (CS online)

### Bulk online subscribers

```http
GET /api/subscribers/online
```

Response:

```json
{
  "subscribers": [
    {
      "imsi": "001010123456789",
      "msisdn": "1234567890",
      "lac": 1234,
      "tmsi": "00A1B2C3",
      "ran": "UTRAN-Iu",
      "state": "online",
      "connected": true
    }
  ]
}
```

### Per-IMSI online (PrettyNMS)

Any of these work:

```http
GET /api/subscribers/online?imsi=<IMSI>
GET /api/subscribers/<IMSI>/online
```

Returns the same `subscribers` array with zero or one entry.

### Subscriber detail

```http
GET /api/subscribers/<IMSI-or-MSISDN>/detail
```

Returns a single object with `imsi`, `msisdn`, `imei`, `lac`, `cell_id`, `ran`,
`lu_complete`, `connected`, `paging`, `active_calls`, and optional `conn_state`.

### Count endpoints

```http
GET /api/subscribers/online/count
GET /api/subscribers/online/count?imsi=<IMSI>
GET /api/subscribers/<IMSI>/online/count
GET /api/subscribers/<IMSI>/detail/count
```

Response:

```json
{"count": 42}
```

Per-IMSI count returns `0` or `1`.

### Disconnect subscriber

```http
DELETE /api/subscribers/<IMSI-or-MSISDN>
```

Response:

```json
{"status":"disconnected","id":"<IMSI>"}
```

---

## Active voice calls

### Bulk active calls

```http
GET /api/calls/active
```

Response:

```json
{
  "calls": [
    {
      "callref": "0x00001234",
      "imsi": "001010123456789",
      "msisdn": "1234567890",
      "direction": "MO",
      "state": "active",
      "transaction_id": 1
    }
  ]
}
```

### Per-IMSI active calls (PrettyNMS)

Any of these work:

```http
GET /api/calls/active?imsi=<IMSI>
GET /api/subscribers/<IMSI>/calls/active
```

Returns the same `calls` array filtered to that subscriber.

### Count endpoints

```http
GET /api/calls/active/count
GET /api/calls/active/count?imsi=<IMSI>
GET /api/subscribers/<IMSI>/calls/active/count
```

Response:

```json
{"count": 3}
```

### Disconnect call

```http
DELETE /api/calls/<callref>/disconnect
```

`callref` accepts decimal or hex (`0x1234`).

Response:

```json
{"status":"disconnected","callref":"0x1234"}
```

---

## MSC links

### List links

```http
GET /api/links
```

Response:

```json
{
  "links": [
    {
      "type": "ran",
      "ran": "GERAN-A",
      "address": "0.23.2",
      "state": "RAN_PEER_ST_READY",
      "connections": 2,
      "osmux": false
    },
    {
      "type": "neighbor",
      "ran": "GERAN-A",
      "target_type": "local_ran_peer",
      "target": "0.23.3",
      "cells": ["lac:100"]
    }
  ],
  "services": {
    "gsup_hlr": {"host": "127.0.0.1", "port": 4222},
    "msc_ipa_name": "MSC-..."
  }
}
```

### Link count

```http
GET /api/links/count
```

Response:

```json
{"count": 5}
```

---

## curl examples

```bash
TOKEN="your-api-token"
HOST="http://10.0.0.1:8080"
AUTH="Authorization: Bearer $TOKEN"

# Bulk online
curl -s -H "$AUTH" "$HOST/api/subscribers/online"

# Per-IMSI online (PrettyNMS)
curl -s -H "$AUTH" "$HOST/api/subscribers/online?imsi=001010123456789"
curl -s -H "$AUTH" "$HOST/api/subscribers/001010123456789/online"

# Per-IMSI in-call (PrettyNMS)
curl -s -H "$AUTH" "$HOST/api/calls/active?imsi=001010123456789"
curl -s -H "$AUTH" "$HOST/api/subscribers/001010123456789/calls/active"

# Counts
curl -s -H "$AUTH" "$HOST/api/subscribers/online/count"
curl -s -H "$AUTH" "$HOST/api/calls/active/count?imsi=001010123456789"
curl -s -H "$AUTH" "$HOST/api/links/count"

# Links
curl -s -H "$AUTH" "$HOST/api/links"
```

---

## Error responses

| HTTP | Meaning |
|------|---------|
| 401 | Missing or invalid token |
| 404 | Unknown path or subscriber/call not found |
| 400 | Invalid subscriber ID or callref |

Error body:

```json
{"error":"description"}
```

---

## Dashboard snapshot (PrettyNMS)

Single poll endpoint for MSC dashboard gauges. Prefer this over parallel `/count` + `/links` calls.

```http
GET /api/stats
```

Response (all fields optional for forward compatibility):

```json
{
  "timestamp": "2026-06-26T12:00:00Z",
  "active_calls": 0,
  "online_subscribers": 0,
  "sms_pending_queue": 0,
  "vlr": { "subscribers": 0 },
  "network": {
    "active_ran_peers": 0,
    "total_ran_peers_seen": 1,
    "active_ss_ussd_sessions": 0
  },
  "sigtran": {
    "asp_up": 1,
    "msu_discarded": 0,
    "msu_rx": 56,
    "msu_tx": 97,
    "asps": [
      { "name": "asp-stp", "rx_packets": 25176, "tx_packets": 25185, "up": true }
    ],
    "application_servers": [
      { "name": "as-stp", "msu_rx": 56, "msu_tx": 97, "msu_discarded": 0 }
    ]
  },
  "sms": {
    "mt_delivery_attempted": 0,
    "mt_delivery_failed_paging": 0,
    "mt_delivery_failed_no_memory": 0
  },
  "calls": {
    "lu_success": 0,
    "mo_setup": 0,
    "reached_active": 0
  }
}
```

| Field | Source |
|-------|--------|
| `active_calls` | `msc.active_calls` stat item |
| `online_subscribers` | VLR subscribers with completed LU (same as `/api/subscribers/online/count`) |
| `sms_pending_queue` | SMS queue `ram:pending` stat item |
| `vlr.subscribers` | VLR subscriber count stat item |
| `network.*` | MSC RAN peer and SS/USSD stat items |
| `sigtran.*` | libosmo-sigtran ASP/AS rate counters |
| `sms.*` | SMS queue delivery rate counters |
| `calls.*` | MSC location-update and call rate counters |

Keep using separate list endpoints for Live / IMSI watch (`/api/subscribers/online`, `/api/calls/active`, etc.).

---

## PrettyNMS integration summary

| Purpose | Endpoint |
|---------|----------|
| **Dashboard snapshot** | `GET /api/stats` |
| CS online per IMSI | `GET /api/subscribers/online?imsi=<IMSI>` or `GET /api/subscribers/<IMSI>/online` |
| CS in-call per IMSI | `GET /api/calls/active?imsi=<IMSI>` or `GET /api/subscribers/<IMSI>/calls/active` |
| Bulk online | `GET /api/subscribers/online` |
| Bulk calls | `GET /api/calls/active` |

All list endpoints have a matching `/count` variant.
