# Class Diagrams
## Runtime Class Diagram
```mermaid
classDiagram
    class tt_Node {
        +uint8_t id
        +uint32_t endpoint_count
        +tt_Endpoint* endpoints[256]
        +uint64_t last_modified
        +tt_UpdateHeader* updates[256]
        +uint8_t tx_buffer[2944]
        +uint32_t tx_tail
        +uint32_t tx_size
        +uint8_t rx_buffer[2944]
        +uint32_t rx_tail
        +uint32_t rx_size
        +tt_TCB scheduler[128]
        +int32_t scheduler_tail
        +tt_hal hal
        +tt_Node_create() int32_t
        +tt_Node_poll() int32_t
        +tt_Node_destroy() int32_t
        +tt_Node_schedule(time, fn, param) bool
    }

    class tt_Endpoint {
        <<embedded base (C struct-cast)>>
        +uint8_t kind
        +uint32_t id
        +const char* name
    }

    class tt_Client {
        +tt_Endpoint endpoint
        +tt_Node* node
        +tt_Service* service
        +tt_CLIENT_CALLBACK callback
        +uint16_t seq_no
        +uint8_t cache_buf[2944]
        +tt_SubmessageHeader* cache
        +uint64_t cache_time
        +uint32_t latency
        +tt_Client_call(request) int32_t
        +tt_Client_destroy() int32_t
    }
    note for tt_Client "cache is NULL when idle, else points into\ncache_buf - fixed storage, no malloc/free\nper call (only one call outstanding at a time)"

    class tt_Server {
        +tt_Endpoint endpoint
        +tt_Node* node
        +tt_Service* service
        +tt_SERVER_CALLBACK callback
        +uint8_t cache_buf[64][2944]
        +tt_SubmessageHeader* cache[64]
        +server_cache_clean_config clean_config[64]
        +bool clean_scheduled[64]
        +tt_Server_destroy() int32_t
    }
    note for tt_Server "cache[i] is NULL when slot i is unused, else\npoints into cache_buf[i]; clean_config[i]/\nclean_scheduled[i] track that slot's retry-\ndedup cleanup timer. Also fixed storage."

    class tt_Publisher {
        +tt_Endpoint endpoint
        +tt_Node* node
        +tt_Topic* topic
        +uint16_t seq_no
        +tt_Publisher_publish(data) int32_t
        +tt_Publisher_destroy() int32_t
    }

    class tt_Subscriber {
        +tt_Endpoint endpoint
        +tt_Node* node
        +tt_Topic* topic
        +tt_SUBSCRIBER_CALLBACK callback
        +uint16_t seq_no
        +tt_Subscriber_destroy() int32_t
    }

    class tt_Service {
        +const char* name
        +uint32_t request_size
        +uint32_t response_size
        +request_encode_size() int32_t
        +request_encode() int32_t
        +request_decode() int32_t
        +request_free() void
        +response_encode_size() int32_t
        +response_encode() int32_t
        +response_decode() int32_t
        +response_free() void
        +uint32_t call_retry_interval
        +uint32_t call_retry_count
    }

    class tt_Topic {
        +const char* name
        +uint32_t data_size
        +data_encode_size() int32_t
        +data_encode() int32_t
        +data_decode() int32_t
        +data_free() void
        +uint16_t history_depth
        +uint32_t deadline_duration
        +uint32_t lifespan_duration
    }

    class tt_TCB {
        <<scheduler task>>
        +uint64_t time
        +function() void
        +void* param
    }

    tt_Endpoint <|-- tt_Client : embed+cast
    tt_Endpoint <|-- tt_Server : embed+cast
    tt_Endpoint <|-- tt_Publisher : embed+cast
    tt_Endpoint <|-- tt_Subscriber : embed+cast

    tt_Node "1" o-- "0..256" tt_Endpoint : endpoints[]
    tt_Node "1" *-- "0..128" tt_TCB : scheduler[]
    tt_Client "0..*" --> "1" tt_Service : service
    tt_Server "0..*" --> "1" tt_Service : service
    tt_Publisher "0..*" --> "1" tt_Topic : topic
    tt_Subscriber "0..*" --> "1" tt_Topic : topic
    tt_Client "*" --> "1" tt_Node : node
    tt_Server "*" --> "1" tt_Node : node
    tt_Publisher "*" --> "1" tt_Node : node
    tt_Subscriber "*" --> "1" tt_Node : node
```

## Protocol Class Diagram
```mermaid
classDiagram
    class tt_Header {
        <<packet header, 4 bytes>>
        +uint16_t magic_value
        +uint8_t version
        +uint8_t source
    }

    class tt_SubmessageHeader {
        <<submessage header, 4 bytes>>
        +uint8_t type
        +uint8_t receiver
        +uint16_t length
    }

    class tt_UpdateHeader {
        <<type=UPDATE>>
        +uint64_t last_modified
        +uint8_t entity_count
    }

    class tt_UpdateEntity {
        +uint32_t endpoint_id
        +uint8_t kind
        +string type
        +string name
    }

    class tt_DataHeader {
        <<type=DATA>>
        +uint32_t endpoint_id
        +uint32_t seq_no
        +uint64_t timestamp
    }

    class tt_CallRequestHeader {
        <<type=CALLREQUEST>>
        +uint32_t endpoint_id
        +uint16_t seq_no
        +uint8_t retry
    }

    class tt_CallResponseHeader {
        <<type=CALLRESPONSE>>
        +uint32_t endpoint_id
        +uint16_t seq_no
        +uint8_t retry
        +int8_t return_code
    }

    tt_Header "1" *-- "1..*" tt_SubmessageHeader : submessages (TLV)
    tt_SubmessageHeader <|.. tt_UpdateHeader : body when type=1
    tt_SubmessageHeader <|.. tt_DataHeader : body when type=2
    tt_SubmessageHeader <|.. tt_CallRequestHeader : body when type=4
    tt_SubmessageHeader <|.. tt_CallResponseHeader : body when type=5
    tt_UpdateHeader "1" *-- "0..*" tt_UpdateEntity : entities[]
```

# Sequence Diagrams
## Publish Sequence Diagram
```mermaid
sequenceDiagram
    participant PubApp as Publisher App
    participant PubTickle as tickle.c (Pub Node)
    participant Net as UDP Broadcast
    participant SubTickle as tickle.c (Sub Node)
    participant SubApp as Subscriber App

    PubApp->>PubTickle: tt_Publisher_publish(pub, data)
    PubTickle->>PubTickle: start_encode(DATA, receiver=ALL)
    PubTickle->>PubTickle: encode(DataHeader: endpoint_id, seq_no, timestamp)
    PubTickle->>PubTickle: topic->data_encode(data → CDR)
    PubTickle->>PubTickle: end_encode(is_flush=false) (4 bytes padding, flush only if needed)
    Note over PubTickle: Publish batches opportunistically: flushed now only if this<br/>submessage doesn't fit tt_MAX_BUFFER_LENGTH, otherwise it<br/>waits for node_flush()'s next tt_NODE_TX_INTERVAL (1ms) tick.<br/>Unlike Call/Response, there's no synchronous waiter to serve.
    PubTickle->>Net: flush_tx() → tt_send() (UDP broadcast)

    Net->>SubTickle: tt_Node_poll() → tt_receive()
    SubTickle->>SubTickle: process_packet() → decode tt_Header
    SubTickle->>SubTickle: process_submessage() → process_data()
    SubTickle->>SubTickle: find_endpoint(TOPIC_SUBSCRIBER, endpoint_id)
    SubTickle->>SubTickle: topic->data_decode(CDR → data)
    alt decode fail
        SubTickle-->>SubTickle: log error and not calling callback
    else decode succeed
        SubTickle->>SubApp: sub->callback(sub, timestamp, seq_no, data)
        SubTickle->>SubTickle: topic->data_free(data)
    end
```

### Call Sequence Diagram
```mermaid
sequenceDiagram
    participant ClientApp as Client App
    participant ClientTickle as tickle.c (Client Node)
    participant Net as UDP Broadcast
    participant ServerTickle as tickle.c (Server Node)
    participant ServerApp as Server App

    ClientApp->>ClientTickle: tt_Client_call(client, request)
    alt client->cache != NULL
        ClientTickle-->>ClientApp: return tt_RET_ILLEGAL_STATUS (-9, wait for response)
    else new call
        ClientTickle->>ClientTickle: start_encode(CALLREQUEST) + CallRequestHeader + service->request_encode
        ClientTickle->>ClientTickle: copy encoded request into cache_buf (for a future retry)
        ClientTickle->>ClientTickle: end_encode(is_flush=true) → immediate flush
        ClientTickle->>ClientTickle: tt_Node_schedule(call_retry, retry_interval)
        ClientTickle->>Net: UDP send (CallRequest)
    end
    Note over ClientTickle,ServerTickle: CallRequest/CallResponse always flush immediately -<br/>the caller is synchronously waiting, so neither leg<br/>waits on node_flush()'s 1ms tick like Publish can.

    Net->>ServerTickle: tt_Node_poll() → process_packet() → process_callrequest()
    ServerTickle->>ServerTickle: find_endpoint(SERVICE_SERVER, endpoint_id)
    alt there is cache (retransmission request)
        ServerTickle->>Net: retransmission (retry++)
    else new call
        ServerTickle->>ServerTickle: service->request_decode (stop if deode error)
        ServerTickle->>ServerApp: server->callback(server, request, response)
        ServerApp-->>ServerTickle: return_code
        ServerTickle->>ServerTickle: CallResponseHeader + response_encode
        ServerTickle->>ServerTickle: set_server_cache() (cache response into a fixed slot, for retry-dedup)
        ServerTickle->>Net: UDP send (CallResponse) - immediate flush
    end

    alt response received in time
        Net->>ClientTickle: process_callresponse()
        ClientTickle->>ClientTickle: service->response_decode (return_code==0)
        ClientTickle->>ClientTickle: update average latency, clear cache (fixed buffer - no free)
        ClientTickle->>ClientApp: client->callback(client, return_code, response)
    else resonse loss → call_retry(TCB) timeout
        ClientTickle->>ClientTickle: retry++ 
        alt retry <= call_retry_count
            ClientTickle->>Net: retransmit cached request
            ClientTickle->>ClientTickle: schedule call_retry
        else out of retry (or schedule fails)
            ClientTickle->>ClientApp: client->callback(client, 0, NULL) — notify failure
            ClientTickle->>ClientTickle: clear cache (fixed buffer - no free)
        end
    end
```

# Performance & Reliability Decisions

A few internal choices exist specifically to keep tail latency and syscall/CPU overhead down.
Noted here since the reasoning isn't obvious from reading any single function in isolation.

## I/O waiting: `poll()`, not `SO_RCVTIMEO`

`tt_receive()` waits for socket readability with `poll()` instead of blocking on `recvfrom()`
with a per-call `SO_RCVTIMEO`. The wait timeout tracks whatever scheduled event
(`tt_Node_schedule()`) is due next, so it changes on nearly every call; re-arming
`SO_RCVTIMEO` via `setsockopt()` that often was pure overhead - measured at ~1255
`setsockopt()` calls for just 30 RPC round trips - for no benefit, since `poll()` takes the
timeout as a plain argument instead. This also sidesteps a real correctness bug the old
approach had: a sub-microsecond nanosecond timeout truncated to `struct timeval{0, 0}` when
converted, and the kernel treats `{0, 0}` as "block forever" for `SO_RCVTIMEO`, not "return
immediately" - the root cause of both an inflated per-request latency and full hangs whenever a
peer disappeared mid-run.

## RPC flushes immediately; Publish batches

`tt_Client_call()`, `call_retry()`, and the server's response send all pass `is_flush=true` to
`end_encode()`: the caller (or the peer waiting on a reply) is synchronously blocked, so
neither leg of an RPC round trip can be left sitting in `tx_buffer` until `node_flush()`'s next
`tt_NODE_TX_INTERVAL` (1ms) tick. `tt_Publisher_publish()` and the periodic `node_update()`
announce still pass `is_flush=false` - pub/sub has no synchronous waiter, so opportunistic
batching (flush only once the buffer would otherwise overflow) is free efficiency with no
latency cost to anyone. This one change dropped measured RPC round-trip latency by ~6.7x (rtt
avg 1.451ms → 0.217ms on the `ping`/`pong` example).

## Fixed-size caches, not `malloc`/`free`

Both `tt_Client.cache` (the one outstanding call) and `tt_Server.cache[]` (up to
`tt_MAX_SERVER_CACHE_COUNT` cached responses, for retry-dedup) are backed by fixed buffers
embedded in the struct (`cache_buf`/`cache_buf[][]`) instead of `malloc()`ing one per
call/response. Both are naturally bounded already (one outstanding call per client; a fixed
slot count per server), so going static adds no unbounded-growth risk - just a larger
`sizeof(struct tt_Server)` (~188KB, dominated by `cache_buf[64][tt_MAX_BUFFER_LENGTH * 2]`),
traded for zero heap churn on the RPC hot path and one less failure mode (no allocation-failure
branch to handle).

## Logging conventions

- `TT_LOG_DEBUG`/`INFO`/`WARNING`/`ERROR` check `tt_current_log_level` *before* calling through
  to `tt_log_debug()` etc., so a suppressed call costs one comparison instead of a full
  variadic call with its format-string arguments already evaluated. `process_packet()`'s decode
  path alone has ~28 `TT_LOG_DEBUG` call sites hit on nearly every received packet, so this
  matters at the default `TT_LOG_INFO` level (measured ~20% less CPU time on a saturated
  receiver). `.clang-tidy` sets `readability-function-cognitive-complexity.IgnoreMacros: true`
  because of the `if` this adds at every call site.
- Never use `perror()` - it bypasses `tt_log_set_output()`/`tt_log_set_level()` entirely (can't
  be redirected or silenced) and doesn't share the timestamp/level formatting the rest of the
  library uses. Use `TT_LOG_ERROR(..., strerror(errno))` for the same information instead.
- A callee that already logs its own specific reason for failing should not have its caller log
  a second, generic message on top of that ("ERROR on data" stacked on top of "Cannot decode
  data for endpoint_id: ..." doesn't add information, just noise).
- "Received malformed/undecodable data from a peer" is logged at `ERROR`, consistently, even
  though the receiving node itself is otherwise healthy - it's still worth an operator's
  attention. "A shared buffer is temporarily full" is `WARNING` when the caller already retries
  automatically (`call_retry()`) or exposes a distinct return code the caller is expected to
  handle gracefully (`tt_RET_OUT_OF_BUFFER` from `tt_Publisher_publish()`).

# Hardware-in-the-Loop CI Architecture

Every push to `main` measures real latency/throughput on physical hardware rather than trusting
a simulated or loopback test - the whole point of a protocol whose stated target is a specific
physical medium (10Base-T1S). See the README's "Continuous performance testing" section for
what it does; this is the *why* behind how it's wired together.

## Topology: orchestrator + two fixed-role DUTs

```
GitHub Actions (cloud)
        |  outbound long-poll (runner dials out; no inbound port needed)
        v
[self-hosted runner, label "tickle-hil"]
        |  SSH (dedicated "ci" account + key, not the operator's own login)
        +----------------+----------------+
        v                                 v
  rpi#1 (client role)              rpi#2 (server role)
  ping / client / publisher        pong / server / subscriber
  / perf_client                    / perf_server
        \_______________ 192.168.10.x test link _______________/
```

The runner never compiles anything or touches the 192.168.10.x link itself - it only dials out
to GitHub and SSHes into the two Pis, which build and run the actual example binaries under
test. That split means the runner's own OS/arch is irrelevant to the measurement, and a
flaky/overloaded runner can't skew a latency result the way it could if the runner *were* one
of the DUTs. Currently the runner is registered on the operator's own development machine
rather than a dedicated third machine - a deliberate simplification for now (one less thing to
provision), revisitable later if that machine's own load ever becomes a concern for
measurement stability; the SSH-key/account boundary to the two Pis already doesn't depend on
which machine the runner happens to live on.

## Why SSH + a plain shell script, not a HIL framework

Labgrid/LAVA/tbot-style device farms solve a harder problem (many boards, flashing images,
serial console capture) than two already-imaged, already-networked Linux boards that just need
a commit checked out and a binary run. `.github/scripts/run_perf.sh` doing
`git fetch && git reset --hard <sha>` + `make` + SSH-launch-and-collect over plain SSH is the
whole mechanism - reused as-is for local debugging outside CI, and with no framework-specific
knowledge required to change it.

## Debounce via `concurrency`, not a cron job

The original ask was "run on push, or after 5 minutes of commit inactivity." Rather than a
separate scheduled workflow, `performance.yml` relies on
`concurrency: {group: perf-main, cancel-in-progress: true}`: a burst of pushes just cancels
each earlier in-progress run as a newer one starts, so only the latest commit's run ever
actually reaches the hardware. (An earlier version of this workflow added a `sleep 300` before
the real work to explicitly wait out 5 minutes of inactivity; dropped once immediate
per-push feedback turned out to matter more than debouncing here.)

## Security: no `pull_request` trigger, ever

`tsnlab/tickle` is a public repository. A self-hosted runner triggered by `pull_request` (or
`pull_request_target`) would let anyone opening a PR from a fork run arbitrary code on hardware
this project's own SSH key can reach - a well-known self-hosted-runner risk on public repos.
`performance.yml` triggers only on `push` (to `main`, which only trusted collaborators can push
to) and `workflow_dispatch`. This is a hard rule for any future workflow using the
`tickle-hil` runner label, not just this one.

## Dedicated non-privileged accounts on the DUTs

Each Pi has its own `ci` account (not the operator's personal login) that the runner's SSH key
is scoped to: a separate, freshly generated key (not reused from anywhere else), the account is
not in `sudo`/`wheel`, and password login is disabled (`passwd -l`) so the key is the only way
in. Compromise of the CI pipeline this way is contained to "can rebuild and run tickle example
binaries as an unprivileged user," not "has the operator's own shell access."
