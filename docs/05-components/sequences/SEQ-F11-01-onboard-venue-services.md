# SEQ-F11-01-onboard-venue-services. Onboard venue: service view

## Type

Service Interaction Sequence

## Feature

- [F-11](../../02-system/features/F-11-external-venues-lob-to-fob/)

## Use Case

- [UC-F11-01](../../02-system/use-cases/UC-F11-01-onboard-venue/use-case.md)

## Purpose

Cross-component поток онбординга площадки: Operator (через Admin UI) → Gateway → cpp/venues Admin API → PostgreSQL venue_config + runtime adapter → внешняя площадка. Реализация — Crow HTTP сервер в [cpp/venues/src/main.cpp](../../../cpp/venues/src/main.cpp).

## Participants

- Operator (через Admin UI / curl)
- cpp/venues Admin API (HTTP `:VENUES_ADMIN_HTTP_PORT`)
- VenuesLoop (in-process)
- PostgresVenueConfigRepository
- External Venues Connector adapter (in-process)
- External Venue (CEX / DEX / AMM)
- cpp/venues VenueObservabilityProducer (Kafka venue.health RAW)

## Diagram

```mermaid
sequenceDiagram
    actor OP as Operator
    participant ADMIN as cpp/venues Admin API
    participant LOOP as VenuesLoop
    participant PG as PostgreSQL venue_config
    participant ADAPT as Venue Adapter (in-proc)
    participant V as External Venue
    participant K as Kafka venue.health

    OP->>ADMIN: POST /api/v1/venues {is_active=false}
    ADMIN->>LOOP: UpsertVenueConfig(record)
    LOOP->>LOOP: apply_runtime_config_locked
    ADMIN->>PG: INSERT venue_config (Upsert)
    PG-->>ADMIN: ok
    ADMIN-->>OP: 200 OK

    OP->>ADMIN: POST /api/v1/venues/{id}/reconnect
    ADMIN->>LOOP: ForceReconnect(venue_id)
    LOOP->>ADAPT: trigger_reconnect
    ADAPT->>V: WS/REST/RPC handshake
    V-->>ADAPT: ok
    ADAPT->>LOOP: heartbeat update
    LOOP->>K: publish VenueHealth (RAW, status=OK)
    ADMIN-->>OP: 200 OK { reconnect triggered }

    OP->>ADMIN: POST /api/v1/venues/{id}/enable
    ADMIN->>LOOP: UpsertVenueConfig(is_active=true)
    LOOP->>ADAPT: start polling/subscribe
    ADMIN->>PG: UPDATE venue_config SET is_active=true
    ADMIN-->>OP: 200 OK { is_active=true }
```

## Contract Binding Table

| Step                              | Transport | Contract                                                              | Location                                                                                                       |
| --------------------------------- | --------- | --------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| Operator → Admin API              | REST/JSON | `POST /api/v1/venues`, `PUT`, `DELETE`, `/reconnect`, `/enable`, `/disable` | [docs/06-api/rest/venues.md](../../06-api/rest/venues.md)                                                      |
| Admin → PostgreSQL                | SQL       | `INSERT … ON CONFLICT … UPDATE venue_config`                          | [docs/07-data/venue-config.md](../../07-data/venue-config.md)                                                  |
| Adapter → External Venue          | venue SDK | venue-native (WS/REST/RPC)                                            | venue-specific (Binance/Coinbase/Uniswap docs)                                                                 |
| Adapter → Kafka                   | Kafka     | `venue.health` (RAW), `fob.venue.v1.VenueHealth`                      | [docs/06-api/messaging/venue-topics.md#venue-health](../../06-api/messaging/venue-topics.md#venue-health)      |

## Data Binding Table

| Data Object              | Storage    | Notes                                                                |
| ------------------------ | ---------- | -------------------------------------------------------------------- |
| `venue_config`           | PostgreSQL | source of truth; hot reload через `apply_runtime_config_locked`      |
| `VenueHeartbeat` (RAW)   | in-memory  | кэш в `VenuesLoop::last_heartbeats_`; экспортируется через `/api/v1/venues/health` |

## Related Components

- [external-venues-connector](../external-venues-connector/overview.md)
- [venue-health-routing](../venue-health-routing/overview.md)
