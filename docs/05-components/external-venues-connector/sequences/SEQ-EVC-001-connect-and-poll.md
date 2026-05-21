# SEQ-EVC-001. Connect + poll cycle (внутренний)

## Type

Internal Component Sequence (external-venues-connector)

## Feature

- [F-11](../../../02-system/features/F-11-external-venues-lob-to-fob/)

## Use Case

- [UC-F11-02](../../../02-system/use-cases/UC-F11-02-publish-snapshot/use-case.md)
- [UC-F11-04](../../../02-system/use-cases/UC-F11-04-venue-health-degradation/use-case.md)

## Purpose

Внутренний жизненный цикл одного venue-adapter'а: connect → subscribe → poll/listen → publish heartbeat → reconnect on error.

## Participants

- VenuesLoop (внутри cpp/venues)
- VenueAdapter (один из: cex_ws_rest / dex_amm_rpc / simulator)
- LocalLobAssembler (для CEX)
- VenueObservabilityProducer
- KafkaProducer venue.health (RAW)

## Diagram

```mermaid
sequenceDiagram
    participant LOOP as VenuesLoop
    participant ADAPT as VenueAdapter
    participant ASSY as LocalLobAssembler (CEX)
    participant OBS as VenueObservabilityProducer

    LOOP->>ADAPT: connect()
    ADAPT-->>LOOP: ok / error

    loop while running
        ADAPT->>ADAPT: poll_once() / await message
        alt CEX
            ADAPT->>ASSY: apply diff
            ASSY-->>ADAPT: snapshot
        else DEX
            ADAPT->>ADAPT: read pool state
        end
        ADAPT-->>LOOP: VenueSnapshot (raw)
        LOOP->>OBS: heartbeat update
        OBS->>OBS: aggregate latency / errors
        OBS->>OBS: publish RAW VenueHealth
        opt error
            ADAPT->>ADAPT: backoff
            ADAPT->>ADAPT: reconnect
        end
    end
```

## Contract Binding Table

| Step                    | Transport | Contract                                    | Location                                                                                                                |
| ----------------------- | --------- | ------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| Adapter ↔ Venue         | venue SDK | venue-native                                | venue docs                                                                                                              |
| OBS → Kafka             | Kafka     | `venue.health` RAW                          | [docs/06-api/messaging/venue-topics.md#venue-health](../../../06-api/messaging/venue-topics.md#venue-health)            |

## Related

- Service sequence: [SEQ-F11-02-publish-snapshot-services](../../sequences/SEQ-F11-02-publish-snapshot-services.md)
