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
      "imsi": "999704281565023",
      "msisdn": "989123456789",
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
      "imsi": "999704281565023",
      "msisdn": "989123456789",
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
curl -s -H "$AUTH" "$HOST/api/subscribers/online?imsi=999704281565023"
curl -s -H "$AUTH" "$HOST/api/subscribers/999704281565023/online"

# Per-IMSI in-call (PrettyNMS)
curl -s -H "$AUTH" "$HOST/api/calls/active?imsi=999704281565023"
curl -s -H "$AUTH" "$HOST/api/subscribers/999704281565023/calls/active"

# Counts
curl -s -H "$AUTH" "$HOST/api/subscribers/online/count"
curl -s -H "$AUTH" "$HOST/api/calls/active/count?imsi=999704281565023"
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

## PrettyNMS integration summary

| Purpose | Endpoint |
|---------|----------|
| CS online per IMSI | `GET /api/subscribers/online?imsi=<IMSI>` or `GET /api/subscribers/<IMSI>/online` |
| CS in-call per IMSI | `GET /api/calls/active?imsi=<IMSI>` or `GET /api/subscribers/<IMSI>/calls/active` |
| Bulk online | `GET /api/subscribers/online` |
| Bulk calls | `GET /api/calls/active` |

All list endpoints have a matching `/count` variant.
