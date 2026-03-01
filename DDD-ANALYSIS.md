# Domain-Driven Design Analysis: strfry

**Version:** 1.0.4-29-g2297edf
**Analysis Date:** 2026-03-01
**Codebase:** ~15,000 lines of C++20 across 45 source/header files

---

## Executive Summary

strfry is a high-performance nostr relay that prioritizes throughput and memory efficiency
over domain clarity. From a Domain-Driven Design perspective, the codebase exhibits a
severely anemic domain model, no bounded context boundaries, no aggregate roots, no
repository pattern, no domain events, and business logic scattered across procedural
functions and thread-pool runners. The architecture is database-driven rather than
domain-driven — the LMDB schema defines the model, and everything else is plumbing.

The relay works. It works fast. But the code is organized around *how* data moves through
threads and *where* bytes live in memory, not around *what* the domain actually means. This
makes the codebase hostile to contributors, resistant to protocol evolution, and fragile
in the face of new NIP requirements.

---

## 1. Ubiquitous Language

DDD requires a shared vocabulary between domain experts and code. The nostr protocol
has clear domain concepts. Here is how strfry represents them:

| Nostr Domain Concept | strfry Representation | Assessment |
|---|---|---|
| Event | 3 separate types: `PackedEventView` (binary blob), `EventToWrite` (DTO), `View_Event` (LMDB view) | Fragmented identity |
| Event ID | `Bytes32`, raw `std::string_view`, or LMDB `uint64_t levId` depending on context | Three identities for one thing |
| Replaceable Event (NIP-16) | `isReplaceableKind()` free function + inline checks on kind ranges | No type, just magic numbers |
| Ephemeral Event (NIP-16) | `if (expiration == 1)` in events.cpp | Magic number, no abstraction |
| Parameterized Replaceable Event (NIP-33) | Scattered d-tag logic in `writeEvents()` | 50+ lines of procedural if/else |
| Event Deletion (NIP-09) | Inline logic in `writeEvents()` checking kind == 5 | No DeleteCommand concept |
| Filter | `NostrFilter` + `NostrFilterGroup` + `FilterSetBytes` + `FilterSetUint` | Four types for one concept |
| Subscription | `Subscription` struct (4 fields, no methods) | Anemic data carrier |
| Connection | `uint64_t connId` | Not even a type alias in most places |
| Relay Information (NIP-11) | String concatenation in `RelayWebsocket.cpp` | Inline HTTP response building |
| Write Policy | `PluginEventSifter` executing external process | Shelled out to another binary |

The codebase does not speak the language of nostr. It speaks the language of LMDB
transactions, thread pool dispatch, and binary memory layouts.

---

## 2. Bounded Contexts

strfry has no explicit bounded contexts. The entire codebase is a single deployment
unit with no internal boundaries. Implicit contexts exist but bleed into each other:

### Implicit Context Map

```
┌─────────────────────────────────────────────────────────┐
│                    strfry (monolith)                     │
│                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐              │
│  │ WebSocket│  │ Event    │  │ Query    │              │
│  │ Protocol │──│ Storage  │──│ Engine   │              │
│  │          │  │          │  │          │              │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘              │
│       │              │              │                    │
│       └──────────────┼──────────────┘                    │
│                      │                                   │
│              ┌───────┴───────┐                           │
│              │ golpe.h       │                           │
│              │ (god header)  │                           │
│              │ env (global)  │                           │
│              │ cfg() (global)│                           │
│              └───────────────┘                           │
│                      │                                   │
│  ┌──────────┐  ┌─────┴────┐  ┌──────────┐              │
│  │ Mesh     │  │ Database │  │ Negentropy│              │
│  │ Sync     │──│ Utils    │──│ Sync     │              │
│  └──────────┘  └──────────┘  └──────────┘              │
└─────────────────────────────────────────────────────────┘
```

Every box in this diagram has direct access to the global `env` LMDB handle and
the global `cfg()` configuration function. There are no anti-corruption layers between
any of these contexts. 62% of all source files (28 out of 45) include the generated
`golpe.h` god header directly.

### Context Boundary Violations

**WebSocket → Event Storage:** `RelayIngester.cpp` directly calls `parseAndVerifyEvent()`
from events.h, which directly calls LMDB operations. There is no command object, no
application service, no indirection. Raw JSON strings flow from the WebSocket handler
into the database layer through a single function call chain.

**Query Engine → Storage:** `DBQuery.h` contains both query logic AND direct LMDB
iteration via `env.generic_foreachFull()`. The query abstraction and the storage
implementation are the same object.

**Mesh Sync → Everything:** `cmd_sync.cpp` includes 10 headers spanning every
implicit context: events, filters, subscriptions, websockets, database queries,
plugins, and negentropy. A single file depends on the entire codebase.

---

## 3. Domain Model Analysis

### 3.1 The Event — An Entity With No Identity

In nostr, an Event is the fundamental entity. It has a cryptographic identity (the
event ID derived from SHA-256 of its canonical form), a lifecycle (creation, storage,
replacement, deletion, expiration), and behavioral rules (replaceable kinds, ephemeral
kinds, parameterized replaceability).

In strfry, the Event is three different things depending on where you are in the code:

**PackedEventView** — A pointer into a binary memory buffer. 88+ bytes: 32 bytes ID,
32 bytes pubkey, 8 bytes created_at, 8 bytes kind, 8 bytes expiration, followed by
variable-length tag data. This is a *view* — it doesn't own its memory. It has accessor
methods but no domain behavior. Defined in `PackedEvent.h` (59 lines).

**EventToWrite** — A data transfer object carrying a packed binary string, a JSON
string, a void pointer for user data, a write status enum, and an LMDB row ID. Five
fields, no methods, no invariants. Defined in `events.h` (11 lines).

**View_Event** — Auto-generated by golpe from `golpe.yaml`. An LMDB record view. The
application code never defines this type; it emerges from code generation.

None of these three representations enforce any business rules. Event validation
happens in free functions: `parseAndVerifyEvent()`, `verifyNostrEvent()`,
`verifyEventTimestamp()`, `verifyNostrEventJsonSize()`, `verifySig()`. The event
doesn't validate itself; external functions validate it. This is the textbook
definition of an anemic domain model.

### 3.2 Event Lifecycle — Procedural Spaghetti

The `writeEvents()` function in `events.cpp` (lines 248–354) is a 100-line procedural
function that handles:

- Duplicate detection (levId lookup by event ID)
- Deletion events (kind 5 processing)
- Replaceable event logic (kind 0, 3, 41, 10000–19999)
- Parameterized replaceable events (kind 30000–39999, d-tag matching)
- Timestamp comparison for replacement decisions
- Event ID comparison for same-timestamp tiebreaking
- Negentropy filter cache invalidation
- LMDB writes for both the event and its payload
- Deletion of replaced/superseded events
- Status updates on the EventToWrite DTOs

This function is doing the work of at least five domain concepts:
1. An EventAcceptancePolicy
2. A ReplacementStrategy
3. A DeletionHandler
4. An EventRepository
5. A CacheInvalidationService

They are all in one function, in one file, with no separation.

### 3.3 Filters and Subscriptions — Value Objects Without Values

A `NostrFilter` should be a value object — immutable, compared by value, self-validating.
Instead, it's a mutable struct populated during construction by parsing JSON, containing
four different `FilterSet` containers (ids, authors, kinds, tags) plus optional
since/until/limit fields. It has no equality operator, no hash function, and its
internal filter sets use raw sorted vectors with binary search.

`NostrFilterGroup` is just a `std::vector<NostrFilter>`. No type safety beyond that.

`Subscription` is a struct with four fields:
```cpp
struct Subscription {
    uint64_t connId;
    SubId subId;
    NostrFilterGroup filterGroup;
    uint64_t latestEventId = 0;
};
```

The `latestEventId` field is mutated from three different locations:
- `QueryScheduler::addSub()` — set when subscription is created
- `RelayReqMonitor::runReqMonitor()` — set when subscription transitions to monitoring
- `ActiveMonitors::process()` — incremented when new events match

This violates value object immutability and makes ownership semantics unclear.
Who owns the subscription? Both `QueryScheduler` and `ActiveMonitors` hold subscriptions,
manage their lifecycle, and mutate their state. There is no aggregate root.

---

## 4. Aggregate Analysis

There are no aggregates in strfry.

The DDD definition: an aggregate is a cluster of domain objects treated as a unit for
data changes, with a single root entity controlling access and enforcing invariants.

strfry has no invariant enforcement at the object level. All business rules are checked
by free functions or inline code in thread pool runners. Objects are accessed and mutated
from multiple contexts without any aggregate boundary.

### Missing Aggregate: Event

An Event aggregate would encapsulate:
- The event's identity (nostr ID)
- Its kind-based behavior (replaceable, ephemeral, parameterized)
- Its validation rules (signature, timestamp, size)
- Its lifecycle transitions (pending → accepted, pending → rejected, stored → deleted)

Instead, events are binary blobs that pass through validation functions.

### Missing Aggregate: Subscription

A Subscription aggregate would encapsulate:
- The subscription's identity (connId + subId)
- Its filter group (immutable after creation)
- Its query state (scanning historical events vs monitoring new ones)
- Its lifecycle (created → querying → monitoring → closed)

Instead, subscriptions are moved between two different containers (`QueryScheduler`
and `ActiveMonitors`) with no unified lifecycle management.

### Missing Aggregate: Connection

A Connection aggregate would encapsulate:
- The connection's identity
- Its active subscriptions
- Its rate limits and resource accounting
- Its IP address and authentication state

Instead, connections are just a `uint64_t connId` scattered across thread pool messages.

---

## 5. Repository Pattern — Absent

There is no repository abstraction. Every component that needs data reaches directly
into the LMDB environment via the global `env` object:

```cpp
// Direct LMDB calls found in 7+ files:
env.lookup_Event(txn, levId);
env.foreach_Event(txn, callback);
env.dbi_EventPayload.get(txn, key, val);
env.insert_Event(txn, packed);
env.delete_Event(txn, levId);
```

Callers in: `events.cpp`, `RelayReqMonitor.cpp`, `ActiveMonitors.h`,
`WriterPipeline.h`, `RelayWriter.cpp`, `cmd_scan.cpp`, `cmd_export.cpp`,
`cmd_compact.cpp`, `cmd_import.cpp`, `cmd_delete.cpp`, `cmd_info.cpp`.

Consequences:
- Storage implementation cannot be changed without modifying every consumer
- No query optimization layer
- Transaction management is implicit and scattered (each caller creates its own)
- Business logic and data access are interleaved

---

## 6. Domain Events — Absent

The nostr protocol is inherently event-driven. Yet strfry has no domain event mechanism.
When an event is written to the database, there is no `EventStored` signal. When an
event replaces another, there is no `EventReplaced` notification. When a deletion
occurs, there is no `EventDeleted` event.

Instead, the system uses two mechanisms that are not domain events:

**File system watching:** A `hoytech::file_change_monitor` watches `data.mdb` for
changes and dispatches wake-up messages to the ReqMonitor thread pool. This means
the domain layer communicates through *filesystem side effects*.

**Callback closures:** Thread pool runners set lambda callbacks on query objects:
```cpp
queries.onEvent = [&](lmdb::txn &txn, const auto &sub, uint64_t levId, ...) { ... };
```
These closures capture `this` from the RelayServer, creating tight coupling between
the query execution engine and the relay's thread dispatch infrastructure.

---

## 7. Anti-Corruption Layers — Absent

### Protocol Boundary

Raw nostr JSON arrives on the WebSocket and flows through the system without
transformation into domain commands. The `MsgIngester::ClientMessage` struct carries
a raw `std::string payload` — the unparsed JSON. Each thread pool runner that handles
this message parses it independently.

There is no `NostrCommand` hierarchy (Subscribe, Publish, Close, etc.). There is no
protocol adapter that translates wire format into domain operations. The WebSocket
frame format and the nostr JSON structure are visible throughout the codebase.

### Database Boundary

LMDB's key-value semantics are visible at every level. Code throughout the system
constructs LMDB keys (`makeKey_StringUint64`, `makeKey_Uint64Uint64`), creates
transactions (`env.txn_ro()`, `env.txn_rw()`), and iterates cursors
(`env.generic_foreachFull()`). The database is not behind any abstraction.

---

## 8. Service Layer — Absent

There are no application services or domain services. Instead:

### Thread Pool Runners as Pseudo-Services

Each thread type has a `run*()` method on `RelayServer`:

| Method | Lines | Responsibilities |
|---|---|---|
| `runIngester()` | 191 | JSON parsing, event verification, write policy, message routing |
| `runReqWorker()` | 64 | Subscription management, query execution, result dispatch |
| `runReqMonitor()` | 56 | File watching, subscription monitoring, event matching |
| `runWriter()` | 82 | Database writing, error handling, response dispatch |
| `runNegentropy()` | 268 | Negentropy protocol handling, sync operations |

These are procedural event loops, not services. Each one mixes:
- Protocol concerns (parsing messages)
- Domain logic (event validation, filter matching)
- Infrastructure concerns (LMDB transactions, thread dispatch)
- Application orchestration (routing between thread pools)

### God Functions

`ingesterProcessEvent()` in `RelayIngester.cpp` (lines 95–150) is a single function that:
1. Checks if event already exists in DB
2. Parses and verifies the event
3. Runs write policy plugin
4. Builds OK response message
5. Constructs writer message for the Writer thread pool
6. Handles all error cases with try/catch

`writeEvents()` in `events.cpp` (lines 248–354) handles five distinct domain operations
in a single function (as detailed in section 3.2).

---

## 9. Thread Architecture Assessment

strfry uses a message-passing architecture with 6 thread pools coordinated by the
`RelayServer` god object:

```
┌─────────────┐
│  WebSocket   │ (1 thread, uWS event loop)
│  I/O         │
└──────┬───────┘
       │ MsgIngester
       ▼
┌─────────────┐     ┌─────────────┐
│  Ingester    │────▶│  Writer      │ (1 thread, exclusive DB writes)
│  (N threads) │     │              │
└──────┬───────┘     └──────────────┘
       │ MsgReqWorker
       ▼
┌─────────────┐
│  ReqWorker   │ (N threads, historical queries)
│              │
└──────┬───────┘
       │ MsgReqMonitor (subscription handoff)
       ▼
┌─────────────┐     ┌─────────────┐
│  ReqMonitor  │     │  Negentropy  │ (N threads)
│  (N threads) │     │              │
└──────────────┘     └──────────────┘

       ┌─────────────┐
       │  Cron        │ (1 thread, periodic cleanup)
       └─────────────┘
```

### Architecture Problems

**Duplicate message types:** `MsgReqWorker` and `MsgReqMonitor` define identical
`NewSub`, `RemoveSub`, and `CloseConn` variants. These are copy-pasted with no shared
base type.

**Subscription ownership split:** A subscription is created in the Ingester, dispatched
to the ReqWorker (which puts it in `QueryScheduler`), then after historical queries
complete, handed off to ReqMonitor (which puts it in `ActiveMonitors`). Two different
containers manage the same conceptual entity with different data structures and different
mutation patterns.

**Ad-hoc dispatch keys:** Thread pool dispatch uses different key strategies:
- Websocket: always dispatches to thread 0
- Ingester/ReqWorker/ReqMonitor: dispatches by `connId`
- Writer: dispatches to thread 0
- Negentropy: dispatches by `connId`

No documentation explains why different pools use different dispatch strategies.

**Callback coupling:** Thread pool runners capture `this` (RelayServer) in lambda
closures to call back into the dispatch infrastructure. This creates circular dependency:
RelayServer owns thread pools, thread pool runners call back into RelayServer.

---

## 10. Configuration Coupling

79 calls to `cfg()` are scattered across the codebase. Configuration is not injected;
it is pulled from a global function. This means:

- Components cannot be tested with different configurations
- Configuration reloads are unsafe (threads may read stale or inconsistent values)
- The config schema is a flat namespace of `section__key` identifiers with no structure

Worst offenders:
- `RelayWebsocket.cpp`: 10+ config reads for HTTP response generation
- `events.cpp`: 6 config reads for validation parameters
- `filters.h`: config read in a header file, affecting every includer

---

## 11. Error Handling Assessment

### Mixed Strategy

The codebase uses exceptions (`hoytech::error()` / `herr()`) as the primary error
mechanism, with 41+ throw sites. However:

- Database lookups return `std::optional<>` — a different error convention
- Plugin communication uses an enum: `PluginEventSifterResult {Accept, Reject, ShadowReject}`
- WebSocket operations return booleans

Three different error strategies in one codebase with no consistency principle.

### Silent Failures

Seven identified locations where errors are logged and swallowed:

1. **Invalid NIP-11 config** (`RelayWebsocket.cpp:55-61`): Malformed `relay.info.nips` JSON
   falls back to default silently
2. **Invalid relay pubkey** (`cmd_relay.cpp:10-16`): Bad hex in config logged as warning,
   relay continues
3. **IP header parsing** (`RelayWebsocket.cpp:216`): Unparseable IP logged, connection continues
4. **TCP keepalive** (`RelayWebsocket.cpp:233`): `setsockopt()` failure logged, ignored
5. **Plugin malformed output** (`PluginEventSifter.h:107`): Unparseable plugin response
   skipped (may loop forever)
6. **Subscription removal** (`QueryScheduler.h:53-59`): RemoveSub on nonexistent subscription
   is a silent no-op
7. **Concurrent deletion** (`events.cpp:342-346`): Already-deleted event silently skipped

---

## 12. Coupling Metrics

### God Header Dependency

28 of 45 files (62%) include `golpe.h` directly. This generated header provides:
- The global `env` LMDB environment
- All database table definitions
- All auto-generated accessor methods
- The `cfg()` configuration function

Changing the database schema requires regenerating this header, which forces
recompilation of 62% of the codebase.

### Include Depth

The critical dependency chain:
```
golpe.h (generated, global state)
  └─ PackedEvent.h (binary format, 59 lines)
       └─ events.h (validation functions, 354 lines)
            └─ filters.h (filter matching, 252 lines)
                 └─ Subscription.h (data carrier, 62 lines)
                      └─ DBQuery.h (query execution)
                           └─ QueryScheduler.h (query batching)
                                └─ RelayServer.h (thread orchestration)
```

Every layer depends on every layer below it. There are no interfaces or abstractions
that would allow substitution at any level.

### Zero Interfaces

The codebase contains zero abstract classes, zero virtual methods, and zero interface
definitions. All types are concrete. All dependencies are on implementations. The only
polymorphism is `std::variant` for message dispatch — which is compile-time, not runtime.

This means:
- No component can be tested in isolation with mocks
- No component can be swapped for an alternative implementation
- The entire system is one monolithic compilation unit in practice

---

## 13. Raw Pointer Hazards

`ActiveMonitors.h` stores raw pointers to `NostrFilter` objects:
```cpp
using MonitorSet = flat_hash_map<NostrFilter*, MonitorItem>;
```

These pointers point into `Subscription::filterGroup` vectors. If a subscription is
destroyed or its filter group is reallocated, these become dangling pointers. The code
acknowledges this with a deleted copy constructor:
```cpp
Monitor(const Monitor&) = delete;  // pointers to filters inside sub must be stable
```

This is a structural hazard that prevents safe refactoring of subscription lifecycle
management.

---

## 14. Anti-Pattern Summary

| Anti-Pattern | Severity | Location |
|---|---|---|
| Anemic Domain Model | Critical | Event, Subscription, Filter — all behavior-free data carriers |
| God Object | Critical | `RelayServer` — owns 6 thread pools, all dispatch logic |
| God Header | High | `golpe.h` — included by 62% of codebase |
| No Repository | High | Direct LMDB calls in 11+ files |
| No Bounded Contexts | High | Single deployment unit, no internal boundaries |
| No Domain Events | High | File system watching instead of domain event propagation |
| No Anti-Corruption Layer | High | Raw JSON/WebSocket protocol visible throughout |
| Feature Envy | Medium | `ingesterProcessEvent()` manipulates event internals |
| Data Clumps | Medium | `EventToWrite` bundles 5 unrelated fields |
| Temporal Coupling | Medium | `latestEventId` must be set before `ActiveMonitors` handoff |
| Long Parameter Lists | Medium | `parseAndVerifyEvent()` — 6 parameters |
| Magic Numbers | Medium | `expiration == 1` for ephemeral, kind ranges hardcoded |
| Silent Failures | Medium | 7 identified swallowed error locations |
| Raw Pointer Hazards | Medium | `NostrFilter*` in `ActiveMonitors` |
| Duplicate Code | Low | `MsgReqWorker` / `MsgReqMonitor` identical variants |

---

## 15. What Would DDD Look Like Here

The nostr relay domain has clear bounded contexts that should be explicit:

### Bounded Contexts

1. **Event Acceptance** — Validating, verifying, and deciding whether to store events
2. **Event Storage** — Persisting events and managing their lifecycle (replacement, deletion, expiration)
3. **Subscription Management** — Managing client subscriptions and their query/monitor lifecycle
4. **Query Execution** — Scanning stored events against filters
5. **Real-Time Monitoring** — Matching incoming events against active subscriptions
6. **Relay Protocol** — WebSocket communication and nostr wire format
7. **Set Reconciliation** — Negentropy-based relay-to-relay sync
8. **Relay Administration** — Configuration, metrics, maintenance

### Aggregate Roots

**Event Aggregate:**
- Identity: EventId (32-byte hash)
- Contains: content, pubkey, kind, tags, signature, timestamps
- Behavior: validates itself, knows if it's replaceable/ephemeral/parameterized
- Invariants: valid signature, valid timestamp range, valid size

**Subscription Aggregate:**
- Identity: (ConnectionId, SubscriptionId)
- Contains: filter group, query state, lifecycle state
- Behavior: transitions between querying → monitoring → closed
- Invariants: valid filter group, valid subscription ID format

**Connection Aggregate:**
- Identity: ConnectionId
- Contains: IP address, active subscriptions, rate limit state
- Behavior: manages subscription lifecycle, enforces limits
- Invariants: max subscriptions per connection, max filters per subscription

### Domain Services

- `EventAcceptanceService` — Coordinates validation, write policy, and storage
- `SubscriptionMatchingService` — Matches events against active subscriptions
- `EventReplacementService` — Handles replaceable/parameterized event logic
- `EventDeletionService` — Handles kind-5 deletion and expiration

### Repository Interfaces

```
EventRepository
  findById(EventId) → Optional<Event>
  findByFilter(Filter, Limit) → Stream<Event>
  save(Event) → Result
  delete(EventId) → Result

SubscriptionRepository
  findByConnection(ConnectionId) → List<Subscription>
  save(Subscription) → void
  remove(ConnectionId, SubscriptionId) → void
```

---

## 16. Conclusion

strfry is infrastructure masquerading as a domain application. It solves the performance
problem (fast event storage and retrieval) while ignoring the domain modeling problem
(what are events, subscriptions, and connections, and what are their rules).

The result is a codebase where:
- Adding a new NIP requires understanding the entire system
- Testing any component requires the full LMDB environment
- Refactoring any layer risks breaking every other layer
- New contributors must understand binary memory layouts, LMDB internals, and thread
  pool dispatch before they can modify business logic

The code is fast. The code is compact. The code is also a monolithic ball of coupled
procedural logic with no domain abstractions, no boundaries, and no path to incremental
improvement without significant restructuring.

This analysis identifies the structural foundation needed to build a relay that is not
only fast but also maintainable, testable, and adaptable to the evolving nostr protocol.
