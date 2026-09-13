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
  "vlr": {
    "subscribers": 0,
    "online": 0,
    "incomplete": 0,
    "incomplete_future": 0,
    "incomplete_never": 0,
    "incomplete_due": 0,
    "incomplete_sgs_lu": 0,
    "incomplete_discarded": 0
  },
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

| Field | Type | What to show / alert |
|-------|------|----------------------|
| `timestamp` | string (UTC ISO-8601) | Sample time |
| `active_calls` | int | Live CS calls |
| `online_subscribers` | uint | Attached (LU complete). Same as `vlr.online` and `/api/subscribers/online/count` |
| `sms_pending_queue` | int | MT SMS waiting in RAM |
| `vlr.subscribers` | int | Every VLR row (attached + leftovers) |
| `vlr.online` | uint | Same as `online_subscribers` |
| `vlr.incomplete` | uint | `vlr.subscribers - vlr.online` (clamped at 0). **Leak / wedge watch** |
| `vlr.incomplete_future` | uint | Incomplete still inside the 10 min LU timer (in-flight or retried SGs LU) |
| `vlr.incomplete_never` | uint | Incomplete with no expiry (open RAN conn, or detach leftover) |
| `vlr.incomplete_due` | uint | Incomplete past expiry, waiting for the sweeper (max 4096/10s) |
| `vlr.incomplete_sgs_lu` | uint | Incomplete still holding the SGs-LU use-count (HLR/MME LU in progress) |
| `vlr.incomplete_discarded` | uint | Incomplete dropped on the last sweeper tick |
| `network.active_ran_peers` | int | BSC/RNC links up |
| `network.total_ran_peers_seen` | int | RAN peers ever seen |
| `network.active_ss_ussd_sessions` | int | Active SS/USSD |
| `sigtran.asp_up` | uint | SIGTRAN ASPs with traffic |
| `sigtran.msu_rx` / `msu_tx` / `msu_discarded` | uint | MSU totals |
| `sigtran.asps[]` | array | Per-ASP name, rx/tx packets, `up` |
| `sigtran.application_servers[]` | array | Per-AS MSU rx/tx/discarded |
| `sms.mt_delivery_attempted` | uint | MT SMS delivery attempts |
| `sms.mt_delivery_failed_paging` | uint | MT SMS paging timeouts |
| `sms.mt_delivery_failed_no_memory` | uint | MT SMS RP memory errors |
| `calls.lu_success` | uint | Successful location updates |
| `calls.mo_setup` | uint | MO call setups |
| `calls.reached_active` | uint | Calls that reached active |

**NMS leak alert:** `vlr.incomplete`. Healthy is hundreds to a few thousand (in-flight LU). Warn above ~10 000 or if it keeps rising while `vlr.online` is flat. The Friday wedge was ~800 000+.

Poll **only** `GET /api/stats` for gauges. Do not poll `/api/subscribers/online` without `?imsi=` (full dump is rejected and used to stall the MSC).

Keep using separate list endpoints for Live / IMSI watch (`/api/subscribers/online?imsi=`, `/api/calls/active`, etc.).

---

## PrettyNMS integration summary

| Purpose | Endpoint |
|---------|----------|
| **Dashboard snapshot** | `GET /api/stats` |
| CS online per IMSI | `GET /api/subscribers/online?imsi=<IMSI>` or `GET /api/subscribers/<IMSI>/online` |
| CS in-call per IMSI | `GET /api/calls/active?imsi=<IMSI>` or `GET /api/subscribers/<IMSI>/calls/active` |
| Bulk online | Do not poll; use `/api/stats`. Single IMSI: `?imsi=` |
| Bulk calls | `GET /api/calls/active` |

All list endpoints have a matching `/count` variant.

---

## IMSI trace packets

`POST /api/trace/<IMSI>` writes DEBUG journal lines plus `PACKET:` dumps. Identity on the wire may be TMSI or MSISDN; the dump is still keyed by the traced IMSI (VLR lookup, no full VLR walk).

| `proto=` | What it is | How it is matched |
|----------|------------|-------------------|
| `sgsap` | SGsAP | IMSI IE in the message |
| `dtap` | A / Iu GSM 04.08 L3 (TMSI on RAN is fine) | VLR subscriber already bound to the connection |
| `gsup` | HLR GSUP | IMSI in GSUP |
| `mncc` | Call control toward SIP/ISUP (SETUP is the MSC-side IAM/INVITE) | MNCC IMSI, called/calling/connected MSISDN, or callref |

OsmoMSC does not terminate SIP or ISUP. `proto=mncc` is the packet to show for those calls. Voice RTP frames are not dumped.
