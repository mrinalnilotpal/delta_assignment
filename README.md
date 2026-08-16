# Order Management & Execution System

A research pipeline publishes target positions `{instrument -> signed target}`.
This layer computes `target - (position + pending)` per instrument and works the
delta into fills while staying correct through the hard parts: partial fills,
cancel/fill races, out-of-sequence events, venue health, and reconciliation.

C++20, CMake, Catch2. No external trading or messaging libraries.

## Build & run

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure   # 84 tests, built with -Werror
./build/sim/oms_sim            # minimal end-to-end demo
./build/sim/oms_bench          # writes results/ (run from the repo root)
./build/sim/oms_signal_demo    # 50-strategy signal pipeline: producer -> gate -> netting
```

```
include/oms/   one header per component (Name.h), plus two cross-cutting files:
               Common.h  - shared std includes, pulled in once via the root headers
               Detail.h  - internal-only helpers (oms::detail), shared by the .cpp files
src/           implementations (Name.cpp)
sim/           deterministic simulated exchange + drivers
tests/         Catch2 tests, one file per area (TestName.cpp)
results/       benchmark output: CSVs + REPORT.md
```

## Design decisions

1. **One writer per event loop, no locks on the order path.** The exchange client
   calls back into the OrderManager synchronously inside `poll()`, on the same
   thread, with no queue in between. A fill updates the order and the position on
   one call stack, so they can never be observed half-applied and there is nothing
   to lock. Concurrency comes from running independent loops, not from sharing
   mutable order state.

2. **The internal order id is the routing table.** It packs
   `[venue:8][generation:16][slot:24][sequence:16]` into 64 bits, so an inbound
   event decodes straight to `pool[slot]` with no hash lookup. The generation
   counter is the safety mechanism: when a slot is reused its generation bumps, so
   a late event carrying the old generation is rejected rather than corrupting
   whichever order owns the slot now.

3. **Terminal state is the dedup boundary, and fills win over cancels.** Three
   terminal states (Filled, Cancelled, Rejected); once terminal an order leaves
   the live index and no event can mutate it. When a fill and a cancel race, the
   fill is applied and the order goes terminal — the cancel-reject that follows is
   expected, tagged a false cancel-reject, and excluded from the health signal.
   This single rule removes most double-counting bugs.

4. **The OMS depends only on interfaces.** It knows `ExchangeClient`, `Router`,
   `HealthModel`, and `MarketDataSource`, never a concrete venue or algo. The
   simulated exchange is just another `ExchangeClient`, and the OMS holds no
   references to concrete algo types.

5. **When a safe action is ambiguous, halt rather than guess.** Auto-heal is used
   only where the corrective action is unambiguous and bounded (a small drift, a
   transient venue blip). A large drift or a fill for an unknown order cannot be
   distinguished from state corruption, so the system hard-stops and waits for an
   operator; guessing at boot or on a large drift can double real risk.

## Architecture

```
  EXECUTION PATH (single-writer event loop)          RECONCILE LOOP (own timer)
  -----------------------------------------          --------------------------
  signals -> SignalGate -> NettingEngine               PeriodicReconciler
             +-------------------+                        - get_positions()
             |  Strategy / Algo  |  TWAP / POV            - drift = exch - oms(filled)
             +--------+----------+                        - band? corrective order
                      | submit(OrderRequest)              - beyond? sticky halt
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

Two independent loops. The reconcile loop only reads positions and, inside a tight
band, submits corrective orders; it never holds the order path and is never held
by it.

## Order lifecycle

Orders come from a fixed-size pool (freelist + per-slot generation counter, pages
committed up front so the first order of the session does not fault on the wire
path). Each order carries internal id, exchange id, instrument, side, size, filled
size, average price, status, and event timestamps. `remaining()` is computed, not
stored, so it cannot drift. Average price is derived from a 128-bit notional
accumulator divided on demand, avoiding floating-point drift.

`pending_cancel` is a flag, not a status, because "partially filled with a cancel
in flight" is a real state a single enum cannot express.

Every inbound handler runs one shared `validate()` first: unknown order (bad or
stale generation), terminal state, duplicate trade id, over-fill, and field
sanity. Anything that is not OK is logged with full context and dropped, never
silently applied. The position updates inside `on_fill` before any subscriber
runs, so no subscriber observes a fill the position does not yet reflect.

Terminal orders leave the live index but keep their pool slot for a short grace
window, so a straggling duplicate resolves to "terminal, dropped" rather than
"unknown, kill switch." Past the window the slot is reclaimed and its generation
bumps, so genuinely stale events correctly read as unknown.

## Exchange interface

`place_order`, `cancel_order`, `get_positions`, `get_order_book`, plus
`set_event_sink` / `poll` / `is_connected`. Sends return a `SendResult` (Ok,
RejectedLocally, TransportDown, RateLimited, Duplicate) rather than a bare bool so
the caller can react. Capability methods such as `mass_cancel` default to "not
supported" so a venue implements only what it has.

Events are delivered synchronously through `ExchangeEventSink` during `poll()`
(see decision 1). `ExchangeRegistry` owns clients by venue id with a
name->constructor factory. `SimulatedExchange` implements the same interface with
seeded control over latency, partial fills, rejects, unsolicited cancels,
reordering, and duplicates; logical time advances per `poll()`, so every run is
reproducible.

## Market data

The system models consumption, not sourcing. `MarketDataSink` receives book/trade
pushes; `MarketDataSource` answers `top_of_book()` and `volume_since()`, the
latter being what POV sizes against.

## Routing & health

Health is three independent signals, each with its own trip and hysteresis:

| Signal | Trip | Recover |
|---|---|---|
| Connectivity | disconnected, or `now - last_seen > tolerance` | on reconnect + fresh data |
| Rejection rate | rate > high over a rolling window (false cancel-rejects excluded) | below low |
| Ack latency | p99 > high over a rolling window | below low |

Health updates from live events, not a poll loop; a timer is used only to notice
"nothing arrived," which requires a clock. State machine: Healthy -> Degraded ->
Down -> (dwell) -> Probing -> Healthy. A venue trips at the high threshold and
recovers only below the low one after a dwell, so a borderline venue does not flap
and thrash routing. On recovery it re-enters at reduced weight and must pass N
clean round trips before full weight.

`Router::select` picks the healthiest tradeable venue, or nothing if all are down.
All-down policy: stop submitting, cancel working orders, mark the cycle
incomplete, and alert. Orders are not buffered, because an order released after
recovery would trade a market that has already moved.

## Execution algorithms

`ExecutionAlgo` is a small event interface (`start`, `on_timer`, `on_fill`,
`on_reject`, `on_cancel`, `cancel`, `is_done`, `stats`). TWAP and POV are
interchangeable because each algo stamps its own index into a scratch field on
every child order and filters the global event stream by that tag; the OMS needs
no algo registry and holds no algo references, and an `AlgoDispatcher` performs
the routing. Selecting an algo is a factory string.

- **TWAP** splits the window into N slices and sizes each
  `ceil(remaining / remaining_slices)`, so partial fills are absorbed
  automatically and a zero slice is skipped rather than sent.
- **POV** sends `rate * volume_observed` per interval. When volume dries up it
  falls back in three stages: idle below a volume floor; if the shortfall grows
  past a threshold, degrade to a held TWAP over the residual window; at the
  deadline, cancel and cross with an IOC, otherwise mark the parent incomplete.

TWAP suits schedule-driven completion; POV suits limiting visible participation
and tolerates under-filling in thin markets.

## Signals & staleness

`SignalMessage { schema_version, sequence, generated_at, strategy_id, targets[] }`
travels over an in-process queue with a length-prefixed binary codec that rejects
truncated buffers. Each message is tagged with a `strategy_id` (a dense index, so
strategies 1..50 share the transport and net independently against overlapping
instruments). `SignalProducer` is a deterministic, seeded stand-in for the research
pipeline — it fabricates target books rather than predicting anything, and drives
`oms_signal_demo` through the full producer -> codec -> gate -> netting path.
Staleness is three checks because signals fail three ways:

| Check | Rule | Action |
|---|---|---|
| Age | `now - generated_at > max_age` | reject |
| Sequence | wrap-aware gap vs expected | forward gap: log and continue; backward: drop |
| Heartbeat | nothing within `interval * threshold` | producer presumed dead: stop new work, cancel resting |

Absurd or NaN targets are rejected at ingest, so a poisoned value never reaches
the delta computation.

## Netting

`NettingEngine` collapses per-strategy deltas into one net order per instrument,
holding the invariant `global == sum of sub-positions`. Fills attribute back
pro-rata by contribution, with the rounding remainder assigned to the largest
contributor (deterministic tie-break). A zero net delta sends no order but still
moves the attributed sub-positions to their targets (an internal transfer) and
records the notional saved; "no order" still requires a bookkeeping update.

## Reconciliation

Source of truth: the exchange for executed reality, local state for intent.

Startup seeds from persisted state, fetches live positions, and stays in
do-not-trade until they reconcile. `SignalGate` boots rejecting every signal until
`enable_trading(can_start)` clears it.

| Condition | Action |
|---|---|
| Match | proceed |
| No persisted state (cold start) | adopt exchange, log |
| Drift <= threshold | adopt exchange (auto-heal), log |
| Drift > threshold | hard stop |
| Exchange unreachable | hard stop |

Steady state: `PeriodicReconciler` runs on its own timer, off the execution path.
It compares the exchange against the OMS filled-only position (in-flight excluded,
or every open order would look like drift), requires the drift to persist across
cycles before acting (so a fill in flight during the slow `get_positions()` is not
chased), emits a `reconciliation`-tagged corrective order inside the auto-heal
band, and sticky-halts beyond it. Corrective orders are excluded from metrics and
netting.

## Failure recovery

Explicit policy per mode; one test each in `TestFailureModes.cpp`.

| # | Mode | Handling |
|---|---|---|
| 1 | No confirmation | ack timer marks the order, attempts a cancel, notifies health; order stays live (never assumed dead) |
| 2 | Fill and cancel-ack out of order | fill wins; the cancel-reject is a false cancel-reject, excluded from health |
| 3 | Unsolicited cancel | applied and logged; the algo requeues the residual |
| 4 | Fill for unknown id | not applied; request a reconcile; hard-stop if it recurs |
| 5 | Duplicate fill | dropped via a per-order trade-id set |
| 6 | Stale fill after slot recycle | rejected by the generation bits |
| 7 | Disconnect mid-flight | keep in-flight orders; reconcile on reconnect |
| 8 | Reject storm | health trips, venue demoted, all-down if every venue trips |
| 9 | Stale or absent signal | age + sequence + heartbeat checks |
| 10 | Pool exhaustion | hard stop with a diagnostic |

Deferred deliberately (out of an 8-hour scope): exchange rate-limit backoff,
multi-leg atomicity, corporate actions, cross-venue aggregation, and journal
recovery after disk corruption.

## Metrics

`MetricsCollector` listens to the OMS event stream and records samples cheaply;
percentiles are computed at report time from a bounded ring, never an unbounded
vector.

- **Slippage** against **arrival mid** (the mid when the OMS accepted the parent),
  which captures the delay cost the execution layer owns; also reported against
  VWAP, which isolates algo skill from market drift. Reporting one without the
  other hides where the cost came from.
- Fill rate, rebalance completion rate, time to fill, ack latency.
- Rejections, ack timeouts, unsolicited cancels, out-of-sequence drops, false
  cancel-rejects, demotions/recoveries, and netting notional saved.

p50/p99/p99.9 per distribution. Caveat: with few samples p99.9 sits on a single
observation, so it indicates shape, not precision. `oms_bench` writes `results/` —
CSVs plus a REPORT.md that interprets the numbers.

## Concurrency

Per decision 1: one writer per loop, the client calls the OMS inline from
`poll()`, and strategies react on the loop thread. Heavy strategy work runs on a
separate loop and hands off at the boundary. The design scales out by running one
loop per venue or shard, shared-nothing.

## Assumptions

- Market data is available; consumption is top-of-book plus interval volume.
- Small instrument universe (tens to low hundreds), one loop.
- No multi-leg atomicity or corporate actions; fees are a simple field, not a schedule.
- Producer and OMS clocks are close enough for the age check; sequence and
  heartbeat cover the rest.
- Signal delivery is at-least-once and may reorder or duplicate; the gate makes it
  effectively exactly-once.

## What I'd do next

1. Real persistence: the fill journal and snapshot that startup reconciliation
   assumes (local state is currently passed in).
2. Live market data into POV and slippage, so arrival- and VWAP-relative slippage
   diverge as they would in a moving market.
3. A multi-venue benchmark so demotion shows up as a routing shift, not only
   all-down.
4. Amend-in-place order flow to reduce cancel/replace churn.
5. Property-based tests for the netting invariant and the id codec.

## Scaling from 5 to 50 strategies

Netting is the first bottleneck: a net delta cannot be computed until every
strategy for that instrument has published, so the slowest or a stalled producer
sets the cycle time. Next, pro-rata rounding residuals accumulate into per-strategy
position error over a day. Then per-cycle work grows with strategies times
overlapping instruments.

The changes that follow: shard event loops by instrument (one owner per instrument
preserves the single-writer property), make netting windowed rather than a hard
barrier, and attribute fills through an exact integer ledger instead of pro-rata
rounding. The trade-off is that sharding by instrument breaks cross-instrument
netting, since one strategy's basket can span shards — resolving it needs either a
cross-shard coordinator (a barrier again) or accepting per-instrument netting. The
pragmatic choice is per-instrument netting, revisited only if un-netted crosses
show up in the slippage numbers.
