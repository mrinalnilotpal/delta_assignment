# Order Management & Execution System

Execution layer for a market-neutral book. Research publishes target positions
`{instrument -> target_quantity}`; this system computes `target - (position +
pending)` and works the delta into fills.

C++20, CMake, Catch2 for tests. No external trading or messaging libraries.

## Build

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure   # 79 tests
./build/sim/oms_sim      # minimal end-to-end demo
./build/sim/oms_bench    # regenerates results/ (run from repo root)
```

Warnings are errors (`-Wall -Wextra -Werror`) on our targets.

## Layout

```
include/oms/   headers, one per component (Name.h)
src/           implementations (Name.cpp)
sim/           deterministic simulated exchange + drivers (oms_sim, oms_bench)
tests/         Catch2 tests, one file per area (TestName.cpp)
results/       benchmark output: CSVs + REPORT.md
```

## Architecture

```
  EXECUTION PATH (single-writer event loop)            RECONCILE LOOP (own timer)
  -----------------------------------------            --------------------------
  signals -> SignalGate -> NettingEngine                 PeriodicReconciler
             +-------------------+                          - get_positions()
             |  Strategy / Algo  |  TWAP / POV              - drift = exch - oms(filled)
             +--------+----------+                          - band? corrective order
                      | submit(OrderRequest)                - beyond? sticky halt
                      v
     +----------------+-----------------+  select   +--------------------+
     |          OrderManager            |--------->|       Router        |
     |  pool, live index, positions     |          +---------+----------+
     |  ExchangeEventSink (on_fill/...) |  events        ^ is_tradeable / score
     +---+-----------+------------------+  <-------- HealthModel
         | subscribe | place/cancel  ^ poll() delivers events on the same stack
         v           v               |
  MetricsCollector   ExchangeRegistry -> ExchangeClient <== SimulatedExchange
                                            ^ pull book / volume
                                            +-- MarketDataSource
```

Two loops. The execution path is a single-writer event loop: one thread reads the
transport, updates order and position state, and calls strategy/algo callbacks on
the same stack. The reconcile loop runs on its own timer, reads positions, and
submits corrective orders when needed; it never blocks the execution path.

Everything depends on interfaces (`ExchangeClient`, `Router`, `HealthModel`,
`MarketDataSource`). The simulated exchange implements the same `ExchangeClient`
as a real venue.

## Concurrency

One writer per loop, so the order path is lock-free by construction. The exchange
client calls back into the OrderManager synchronously from `poll()` with no queue
between them, so a fill updates the order and position on one stack and can't be
seen half-applied. Strategies react to published events on the loop thread; if
they need their own work, they run a separate loop and hand off at the boundary.
Scale by running one loop per venue/shard, shared-nothing.

## Order lifecycle (2.3)

Orders come from a fixed-size pool (freelist + per-slot generation counter, pages
committed up front so there's no allocation on the wire path). The internal id
packs routing into 64 bits so inbound events decode without a hash lookup:

```
[venue:8][generation:16][slot:24][sequence:16]
```

- venue routes the event to the right client.
- slot indexes the pool directly.
- generation bumps on slot reuse, so a late event for a recycled slot is rejected
  instead of hitting the live order now in that slot.

Status is one byte with three terminal states (Filled, Cancelled, Rejected).
`pending_cancel` is a separate flag, not a status, so an order can be partially
filled with a cancel in flight.

Every inbound handler starts with one `validate()` call covering: unknown order,
terminal state, duplicate trade id, over-fill, and field sanity. Anything not OK
is logged with context and dropped, never applied. Other rules:

- Fills win over cancels. A fill that completes an order while a cancel is in
  flight goes terminal; the following cancel-reject is a false cancel-reject,
  counted and kept out of the health rejection rate.
- Positions update inside `on_fill` before any subscriber runs, so nobody sees a
  fill before the position reflects it.
- A fill for an unknown order means the position is already wrong: log, request a
  reconcile, and hard-stop if it recurs.
- Terminal orders leave the live index but their slot is freed only after a grace
  window, so recent duplicate/late events drop cleanly instead of looking unknown.

Prices are integer ticks (`int64_t`), never doubles. Average price is
`total_fill_amount / filled_size`, with the notional in a 128-bit accumulator.

## Exchange interface (2.4)

`ExchangeClient` exposes `place_order`, `cancel_order`, `get_positions`,
`get_order_book`, `set_event_sink`, `poll`, `is_connected`. Capability methods
(e.g. `mass_cancel`) default to "not supported". Sends return a `SendResult` (Ok,
RejectedLocally, TransportDown, RateLimited, Duplicate). Events arrive
synchronously through `ExchangeEventSink` during `poll()` (see Concurrency).
`ExchangeRegistry` owns clients by `VenueId` and has a name->constructor factory.

`SimulatedExchange` implements the same interface with seeded control over
latency, partial fills, rejects, unsolicited cancels, reordering, duplicates, and
disconnects. Logical time advances per `poll()`, so runs are reproducible.

## Market data (2.5)

`MarketDataSink` receives book/trade pushes; `MarketDataSource` answers
`top_of_book()` and `volume_since()` (what POV consumes). We model consumption,
not sourcing.

## Routing & health (2.6)

Health combines three signals, each with hysteresis:

| Signal | Trip | Recover |
|---|---|---|
| Connectivity | disconnected, or `now - last_seen > tolerance` | on reconnect + fresh data |
| Rejection rate | rate > high over a rolling window (false cancel-rejects excluded) | below low |
| Ack latency | p99 > high over a rolling window | below low |

Counters update from events, not polling; a timer only checks time-based
staleness. States: Healthy -> Degraded -> Down -> (dwell) -> Probing -> Healthy.
A recovered venue re-enters at reduced weight and must pass N round trips before
full weight, so it doesn't flap.

`Router::select` returns the healthiest tradeable venue, or nothing if all are
down. When all are down the OMS stops submitting, cancels working orders, marks
the cycle incomplete, and alerts. It does not buffer, because a delayed order
would trade a market that has moved.

## Execution algos (2.7)

`ExecutionAlgo` is a small interface (`start`, `on_timer`, `on_fill`, `on_reject`,
`on_cancel`, `cancel`, `is_done`, `stats`). TWAP and POV both implement it, and
swapping them needs no OrderManager change.

They stay decoupled by tagging: an algo stamps its index into a scratch field on
each child order and filters the global event stream by it. The OrderManager has
no algo registry and no references to `TwapAlgo`/`PovAlgo`; an `AlgoDispatcher`
does the match. Algos are created by name via `AlgoFactory`.

- TWAP: split `[start, end]` into N slices; each slice sends
  `ceil(remaining / remaining_slices)`, so partial fills are absorbed
  automatically. Zero slices are skipped.
- POV: each interval sends `rate * volume_observed`. When volume dries up:
  (1) below a floor, idle; (2) shortfall past a threshold, degrade to a held TWAP
  over the residual window; (3) at the deadline, cancel and cross with an IOC,
  else mark the parent incomplete.

## Signals & staleness (2.8)

`SignalMessage { schema_version, sequence, generated_at, strategy_id, targets[] }`,
carried over an in-process queue with a length-prefixed binary codec that rejects
truncated buffers. Three staleness checks:

| Check | Rule | Action |
|---|---|---|
| Age | `now - generated_at > max_age` | reject |
| Sequence | wrap-aware gap vs expected | forward gap: log and continue; backward: drop |
| Heartbeat | nothing within `interval * threshold` | producer presumed dead: stop new work, cancel resting |

NaN/absurd targets are rejected at ingest.

## Netting (2.9)

`NettingEngine` sums per-strategy deltas into one net order per instrument,
keeping `global_position == sum of sub_positions`. Fills are attributed pro-rata
by contribution, with the rounding residual going to the largest contributor.
When the net is zero but strategy deltas aren't, it sends nothing but still moves
attributed positions (an internal transfer) and records the notional saved.

## Reconciliation (2.10)

Source of truth: exchange for executed reality, local state for intent.

Startup: seed from persisted local state, fetch exchange positions, and stay in
do-not-trade until they reconcile. `SignalGate` boots in DNT and rejects every
signal until `enable_trading(can_start)` clears it.

| Condition | Action |
|---|---|
| Match | proceed |
| No persisted state | adopt exchange (cold start), log |
| Drift <= threshold | adopt exchange (auto-heal), log |
| Drift > threshold | hard stop |
| Exchange unreachable | hard stop |

Steady state: `PeriodicReconciler` runs on its own timer. It compares exchange
positions to the OMS filled-only position (in-flight excluded), requires the
drift to persist across cycles before acting, emits a `reconciliation`-tagged
corrective order inside the auto-heal band, and does a sticky halt (manual
re-arm) beyond it. Corrective orders are excluded from metrics and netting.

## Failure recovery (2.11)

One test per mode in `TestFailureModes.cpp`.

| # | Mode | Handling |
|---|---|---|
| 1 | No confirmation | ack timer marks the order, attempts a cancel, tells health; order stays live (not assumed dead) |
| 2 | Fill/cancel-ack out of order | fill wins; the cancel-reject is a false cancel-reject, excluded from health |
| 3 | Unsolicited cancel | applied and logged; the algo requeues the residual |
| 4 | Fill for unknown id | not applied; request reconcile; hard-stop if it recurs |
| 5 | Duplicate fill | dropped via a per-order trade-id set |
| 6 | Stale fill after recycle | rejected by the generation bits |
| 7 | Disconnect mid-flight | keep in-flight orders; reconcile on reconnect |
| 8 | Reject storm | health trips, venue demoted, all-down if every venue trips |
| 9 | Stale/absent signal | age + sequence + heartbeat checks |
| 10 | Pool exhaustion | hard stop with a diagnostic |

Auto-heal only when the safe action is unambiguous and bounded; otherwise halt
and let a human decide. Deferred, out of scope but not forgotten: exchange
rate-limit backoff, multi-leg atomicity, corporate actions, cross-venue
aggregation, and journal recovery after disk corruption.

## Metrics (2.12)

`MetricsCollector` subscribes to OMS events and records samples cheaply;
percentiles are computed at report time from a fixed-size ring (bounded memory).

- Slippage vs arrival mid (cost of the execution layer's delay) and vs VWAP (algo
  skill), in bps.
- Fill rate, rebalance completion rate, time to fill, ack latency.
- Rejections, ack timeouts, unsolicited cancels, out-of-sequence drops, false
  cancel-rejects, demotions, and netting notional saved.

p50/p99/p99.9 for each distribution. With few samples p99.9 collapses onto one
observation, so it's there for shape, not precision.

`./build/sim/oms_bench` runs a deterministic multi-cycle run with both algos and
injected failures and writes `metrics_summary.csv`, `cycles.csv`, and a
`REPORT.md` that interprets the numbers.

## Tests (2.13)

79 Catch2 tests, one file per area, all against the seeded sim: order lifecycle
and out-of-sequence events, id encode/decode, the ten failure modes,
routing/health, TWAP/POV under partial fills, the netting invariant, and
reconciliation drift.

## Assumptions

- Market data is available; we model consumption at top-of-book + interval volume.
- Small instrument universe (tens to low hundreds), one loop.
- No multi-leg atomicity or corporate actions; simplified fees.
- Producer and OMS clocks are close enough for age checks; sequence and heartbeat
  cover the rest.
- Signal delivery is at-least-once (may reorder or duplicate); the gate makes it
  effectively exactly-once.

## What I'd do next

1. Persistence: the fill journal + snapshot that startup reconciliation assumes.
2. Live market data into POV/slippage so VWAP- and arrival-relative slippage
   diverge as they would in a moving market.
3. Multi-venue benchmark so demotion shows as a routing shift, not just all-down.
4. Amend-in-place order flow to cut cancel/replace churn.
5. Property-based tests for the netting invariant and the id codec.

## Scaling 5 -> 50 strategies

The netting cycle breaks first: a net delta needs every strategy for that
instrument to have published, so the slowest (or a stalled) producer sets the
cycle time. Next, pro-rata rounding residuals accumulate into per-strategy
position error over a day. Then per-cycle work grows with strategies times
overlapping instruments.

What I'd change: shard event loops by instrument (one owner per instrument keeps
single-writer), make netting windowed instead of barrier-synchronous, and
attribute fills through an exact integer ledger instead of pro-rata rounding. The
trade-off is that sharding by instrument breaks cross-instrument netting, since
one strategy's basket can span shards; resolving it needs a cross-shard
coordinator (a barrier again) or accepting per-instrument netting.
