# Dante Control, Audio, and Video Protocol

## Scope

This document defines the integration protocol between a Dante DVR control
service and an audio/video application endpoint. It covers only the protocol
surfaces:

- Unix-domain control messages for video-flow routing and status.
- H.264 video carried over RTP/UDP using the routing information.
- Dante DEP PCM audio carried through shared memory and a timing object.

It does not define application internals, SDK APIs, callback behavior, thread
models, implementation limits, or product-specific configuration.

## Terms

| Term | Meaning |
| --- | --- |
| DVR | The Dante DVR control service that creates/removes video routes. |
| Endpoint | An application that provides or consumes Dante-associated video and audio. |
| Flow | One routed RTP video connection identified by `flowIndex`. |
| Channel | A logical endpoint video channel identified by `channelIndex`. |
| TX | Media transmitted by the endpoint. |
| RX | Media received by the endpoint. |
| DEP | Dante Embedded Platform audio shared-memory and timing interface. |

## Protocol Planes

The integration uses three separate planes. Control messages never carry media
payloads.

```text
                 Unix SOCK_SEQPACKET JSON
Endpoint <---------------------------------> DVR
   |                                           |
   | RTP/UDP H.264                             | route allocation
   +-------------------------------------------+

Endpoint <-------- DEP shared memory --------> Dante DEP
                 PCM32 audio + period timing
```

| Plane | Transport | Payload | Purpose |
| --- | --- | --- | --- |
| Control | Unix-domain `SOCK_SEQPACKET` | UTF-8 JSON object | Capability announcement, route creation/removal, route status. |
| Video | UDP/RTP | H.264 | Video media for an allocated TX or RX flow. |
| Audio | DEP shared memory and timing object | Non-interleaved PCM32 | Dante audio exchange. |

## Control Transport

### Endpoint

The control endpoint is the Unix-domain socket:

```text
/var/run/dante/dvr
```

The application endpoint connects to the DVR. The socket type is
`AF_UNIX`/`SOCK_SEQPACKET`.

### Record Rules

- One `SOCK_SEQPACKET` record contains exactly one complete JSON object.
- JSON text is UTF-8.
- A record has no NUL terminator on the wire.
- A receiver must parse the received record length, rather than treating an
  embedded or trailing NUL as framing.
- No stream framing, newline delimiter, or application length prefix is used.
- The protocol has no version field. Participants must reject unsupported
  actions and parameter shapes.

### Common JSON Envelope

Every control record has this form:

```json
{
  "action": "<action-name>",
  "parameters": { }
}
```

`action` is a case-sensitive string. `parameters` is always an object, even
when the action needs no parameters.

## Session Messages

### Start

The endpoint sends `start` after connecting. It advertises whether it supports
at least one H.264 TX and/or RX video channel.

```json
{
  "action": "start",
  "parameters": {
    "txVideoChannels": [
      { "subtypes": ["H264"] }
    ],
    "rxVideoChannels": [
      { "subtypes": ["H264"] }
    ]
  }
}
```

Use an empty array for an unsupported direction:

```json
{
  "action": "start",
  "parameters": {
    "txVideoChannels": [],
    "rxVideoChannels": [
      { "subtypes": ["H264"] }
    ]
  }
}
```

`subtypes` currently identifies H.264 support with the literal `"H264"`.

### Stop

The endpoint sends `stop` before closing the control connection:

```json
{
  "action": "stop",
  "parameters": {}
}
```

### Configuration Request

The DVR may send a configuration request:

```json
{
  "action": "requestConfiguration",
  "parameters": {}
}
```

This action carries no routing information. A participant may ignore it when
the start advertisement is its complete configuration response.

## Video Routing Messages

The DVR creates and removes video flows. `flowIndex` identifies the route;
`channelIndex` identifies the endpoint's logical video channel.

### Unicast TX Flow

The DVR directs the endpoint to transmit H.264 RTP to `receiverAddress:port`.

```json
{
  "action": "createVideoUnicastTxFlow",
  "parameters": {
    "flowIndex": 0,
    "channelIndex": 0,
    "port": 5004,
    "receiverAddress": "192.0.2.20",
    "transmitterAddress": "192.0.2.10"
  }
}
```

### Multicast TX Flow

The DVR directs the endpoint to transmit H.264 RTP to
`multicastAddress:port`.

```json
{
  "action": "createVideoMulticastTxFlow",
  "parameters": {
    "flowIndex": 0,
    "channelIndex": 0,
    "port": 5004,
    "multicastAddress": "239.192.0.20",
    "transmitterAddress": "192.0.2.10"
  }
}
```

### Unicast RX Flow

The DVR directs the endpoint to receive H.264 RTP at
`receiverAddress:port`, accepting media from `transmitterAddress`.

```json
{
  "action": "createVideoUnicastRxFlow",
  "parameters": {
    "flowIndex": 0,
    "channelIndex": 0,
    "port": 5004,
    "receiverAddress": "192.0.2.20",
    "transmitterAddress": "192.0.2.10"
  }
}
```

### Multicast RX Flow

The DVR directs the endpoint to join `multicastAddress` and receive H.264 RTP
on `port`, accepting media from `transmitterAddress`.

```json
{
  "action": "createVideoMulticastRxFlow",
  "parameters": {
    "flowIndex": 0,
    "channelIndex": 0,
    "port": 5004,
    "multicastAddress": "239.192.0.20",
    "transmitterAddress": "192.0.2.10"
  }
}
```

### Delete TX Flow

```json
{
  "action": "deleteTxFlow",
  "parameters": {
    "flowIndex": 0
  }
}
```

The endpoint must stop RTP transmission for that flow.

### Delete RX Flow

```json
{
  "action": "deleteRxFlow",
  "parameters": {
    "flowIndex": 0
  }
}
```

The endpoint must stop receiving and release the corresponding RTP route.

### Routing Parameter Contract

| Parameter | Type | Required by | Meaning |
| --- | --- | --- | --- |
| `flowIndex` | unsigned integer | all create/delete actions | DVR route identity. |
| `channelIndex` | unsigned integer | all create actions | Endpoint logical video channel. |
| `port` | integer, 1-65535 | all create actions | UDP RTP port. |
| `receiverAddress` | IP address string | unicast create actions | RTP destination for TX; local receiving address for RX. |
| `multicastAddress` | IPv4 multicast address string | multicast create actions | RTP multicast destination or group to join. |
| `transmitterAddress` | IP address string | all create actions | Source address of the RTP transmitter. |

## Flow Status Messages

The endpoint reports route health to the DVR over the control socket.

### RX Flow Status

```json
{
  "action": "reportRxFlowStatus",
  "parameters": {
    "flowIndex": 0,
    "flowStatus": "statusOK"
  }
}
```

Supported `flowStatus` values:

| Value | Meaning |
| --- | --- |
| `statusOK` | RTP packets are being received for the flow. |
| `statusNotReceivingPackets` | The flow exists but RTP packets are not being received. |

### TX Channel Status

```json
{
  "action": "reportTxChannelStatus",
  "parameters": {
    "channelIndex": 0,
    "channelStatus": "statusOK"
  }
}
```

Supported `channelStatus` values:

| Value | Meaning |
| --- | --- |
| `statusOK` | The endpoint TX channel is connected and usable. |
| `statusExtNotConnected` | The external receiver is not connected. |

## Video Media Protocol

### Transport

- Media transport is UDP/RTP.
- Video codec is H.264.
- The control-plane create action supplies the address, port, direction, and
  unicast or multicast mode.
- A TX endpoint sends RTP packets only while its TX flow exists.
- An RX endpoint accepts RTP packets only while its RX flow exists and uses the
  configured transmitter address as the source identity.

### Flow Relationship

```text
createVideo*TxFlow
  -> endpoint packetizes H.264 as RTP
  -> sends UDP RTP to configured unicast receiver or multicast group

createVideo*RxFlow
  -> endpoint binds or joins the configured UDP destination
  -> receives and depacketizes H.264 RTP from configured transmitter
```

The control protocol establishes routing only. RTP packetization, H.264 access
unit construction, payload type, SSRC, sequence numbers, timestamps, and SDP
or other session-description exchange are outside this control JSON contract.

## Audio Media Protocol

### Overview

Dante audio does not use the control socket or the H.264 RTP video flow. It is
exchanged through the Dante DEP shared-memory buffer interface and a timing
object signalled once per audio period.

```text
DEP TX channel buffers: endpoint writes samples for Dante transmission
DEP RX channel buffers: endpoint reads samples received by Dante
```

Channel buffers are non-interleaved: every channel has an independent sample
ring.

### Shared-Memory Names

The primary DEP shared-memory object is named:

```text
DanteEP
```

When the header sets `DANTE_BUFFERS_FLAG__SEPARATE_CHANNEL_MEMORY`, TX and RX
audio channel regions are mapped from separate objects:

```text
DanteEPTx
DanteEPRx
```

Without that flag, metadata and all channel buffers are sections of one
continuous `DanteEP` mapping.

### Header Validity and Layout

The shared memory starts with `buffer_header_t`.

| Header field | Meaning |
| --- | --- |
| `metadata.magic_marker` | `DANTE_BUFFERS_HDR_MAGIC` (`0x50525354`) means the mapping is configured and valid. Zero means unavailable, configuring, or invalid. |
| `metadata.buffer_length` | Total mapping length in bytes. |
| `metadata.metadata_header_length` | Metadata block length. |
| `metadata.flags` | Shared-memory layout and timing-update flags. |
| `metadata.first_tx_channel_offset_bytes` | Offset from header start to TX channel 0. |
| `metadata.first_rx_channel_offset_bytes` | Offset from header start to RX channel 0. |
| `metadata.timing_object_subheader_offset_bytes` | Offset to the timing-object descriptor. |
| `metadata.reset_count` | Changes across a DEP buffer reset. |

Audio format fields:

| Header field | Meaning |
| --- | --- |
| `audio.sample_rate` | Active device sample rate. Zero means a sample-rate transition is in progress. |
| `audio.encoding` | `DANTE_ENCODING__PCM32` (`32`) for PCM32; `DANTE_ENCODING__FLOAT32` (`65`) is also defined by the ABI. The active DEP audio encoding is PCM32. |
| `audio.samples_per_channel` | Number of sample positions in each channel ring. |
| `audio.bytes_per_channel` | Byte length of each channel ring. |
| `audio.num_tx_channels` | Available TX channel count. |
| `audio.num_rx_channels` | Available RX channel count. |

### Period Timing

The timing fields establish the transfer cadence:

| Header field | Meaning |
| --- | --- |
| `time.samples_per_period` | Number of samples processed per timing period. |
| `time.period_count` | Monotonically increasing count of elapsed periods. |
| `time.epoch_seconds`, `time.epoch_samples` | PTP epoch associated with synchronisation. |
| `time.monotonic` | Local monotonic time at the current period count. |

The timing-object descriptor is located using
`timing_object_subheader_offset_bytes`. It identifies a timing object with:

```text
object_type = TIMING_OBJECT_TYPE__SIGNAL_EVENT
object_name = POSIX semaphore name on POSIX platforms
```

DEP signals the timing object after every audio period. An endpoint wakes,
reads the current `period_count`, and transfers every elapsed period. If more
than one period elapsed since the previous wakeup, it must process each missed
period in order.

### Memory Ordering and Reset

DEP writes audio data before publishing its corresponding period update. After
observing a changed `period_count`, an endpoint must apply an acquire memory
barrier before reading RX samples or relying on updated metadata.

`reset_count` changes at the start and completion of a buffer reset. An endpoint
must treat a reset as a discontinuity:

1. Stop using channel pointers and pending ring positions.
2. Re-read metadata after the header is valid again.
3. Reopen/re-evaluate the timing object if its descriptor changed.
4. Resume transfer from the current `period_count`.

For the `time.monotonic` and `time.period_count` pair, DEP brackets a multi-field
update with `DANTE_BUFFERS_FLAG__TIMING_UPDATE_SYNC`. A reader requiring a
consistent pair must avoid using the pair while this flag is set and recheck it
after reading both values.

### Audio Transfer Rules

- TX samples are written to the selected non-interleaved TX channel ring.
- RX samples are read from the selected non-interleaved RX channel ring.
- Each transfer period contains `samples_per_period` samples per participating
  channel.
- Ring positions wrap modulo `samples_per_channel`.
- Channel indices are zero based and must be less than the corresponding
  `num_tx_channels` or `num_rx_channels` value.
- Sample rate, encoding, channel count, ring capacity, and timing metadata are
  authoritative only while `magic_marker` is valid and no reset is in progress.

## Control and Media State Summary

```text
Control connection established
  -> start capability advertisement
  -> zero or more createVideo*Flow messages
  -> RTP video active for each created flow
  -> report flow/channel status as health changes
  -> delete*Flow stops the matching RTP media route
  -> stop ends the control session

DEP mapping valid
  -> wait for each timing signal
  -> transfer PCM32 TX/RX periods
  -> reset_count or invalid magic ends the current mapping epoch
  -> remap/reinitialise and resume with new metadata
```
