# Order Management & Execution System

Research publishes target positions `{instrument -> signed target}`. This layer
computes `target - (position + pending)` per instrument and works the delta into
fills while staying correct through the messy parts: partial fills, cancel/fill
races, out-of-sequence events, venue health, and reconciliation.

C++20, CMake, Catch2. No external trading or messaging libraries.

## Build & run

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure   # 79 tests, built with -Werror
./build/sim/oms_sim     # minimal end-to-end demo
./build/sim/oms_bench   # writes results/ (run from the repo root)
```

```
include/oms/   headers, one per component (Name.h)
src/           implementations (Name.cpp)
sim/           deterministic simulated exchange + drivers
tests/         Catch2 tests, one file per area (TestName.cpp)
results/       benchmark output: CSVs + REPORT.md
```

## Design decisions that matter

The ones I'd defend on the call:

1. **One writer per event loop, no locks on the order path.** The exchange client
   calls back into the OrderManager synchronously inside `poll()`, on the same
   thread, with no queue in between. A fill updates the order and the position on
   one call stack, so they can't be seen half-applied and there's nothing to lock.
   Concurrency comes from running independent loops, not from sharing mutable
   order state.

2. **The internal order id is the routing table.** It packs
   `[venue:8][generation:16][slot:24][sequence:16]` into 64 bits, so an inbound
   event decodes straight to `pool[slot]` with no hash lookup. The generation
   counter is the safety catch: when a slot is reused its generation bumps, so a
   late event carrying the old generation is rejected instead of corrupting
   whoever owns the slot now.

3. **Terminal state is the dedup boundary, and fills win over cancels.** Three
   terminal states (Filled, Cancelled, Rejected); once terminal an order leaves
   the live index and no event can touch it. If a fill and a cancel race, the fill
   is applied and the order goes terminal — the cancel-reject that follows is
   expected, tagged a false cancel-reject, and kept out of the health signal. This
   one rule removes most double-counting bugs.

4. **The OMS depends only on interfaces.** It knows `ExchangeClient`, `Router`,
   `HealthModel`, `MarketDataSource` — never a concrete venue or algo. The
   simulated exchange is just another `ExchangeClient`; grep the OMS for
   `TwapAlgo`/`PovAlgo` and you find nothing.

5. **When in doubt, halt — don't guess.** Auto-heal only where the safe action is
   unambiguous and bounded (a small drift, a transient venue blip). A large drift
   or a fill for an unknown order can't be told apart from corruption, so the
   system hard-stops and waits for a human. Guessing at boot or on a big drift can
   double real risk.

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

Orders live in a fixed-size pool (freelist + per-slot generation, pages committed
up front so the first order of the day doesn't fault on the wire path). Per order
I track internal id, exchange id, instrument, side, size, filled size, average
price, status, and event timestamps. `remaining()` is computed, never stored, so
it can't drift. Average price comes from a 128-bit notional accumulator divided on
demand — no floating average, no cent drift.

`pending_cancel` is a flag, not a status, because "partially filled and has a
cancel in flight" is a real state a single enum can't express.

Every inbound handler runs one shared `validate()` first: unknown order (bad or
stale generation), terminal state, duplicate trade id, over-fill, field sanity.
Anything that isn't OK is logged with full context and dropped, never silently
applied. The position updates inside `on_fill` before any subscriber runs, so
nobody sees a fill the position doesn't yet reflect.

Terminal orders leave the live index but keep their pool slot for a short grace
window, so a straggling duplicate resolves to "terminal, dropped" rather than
"unknown, kill switch." Past the window the slot is reclaimed and its generation
bumps, so genuinely stale events correctly read as unknown.

## Exchange interface

`place_order`, `cancel_order`, `get_positions`, `get_order_book`, plus
`set_event_sink` / `poll` / `is_connected`. Sends return a `SendResult` (Ok,
RejectedLocally, TransportDown, RateLimited, Duplicate) rather than a bare bool,
so the caller can actually react. Capability methods like `mass_cancel` default to
"not supported" so a venue only implements what it has.

Events are delivered synchronously through `ExchangeEventSink` during `poll()` —
deliberate, for the reason in decision 1. `ExchangeRegistry` owns clients by venue
id with a name->constructor factory. `SimulatedExchange` implements the same
interface with seeded control over latency, partial fills, rejects, unsolicited
cancels, reordering, and duplicates; logical time advances per `poll()`, so every
run is reproducible.

## Market data

I model consumption, not sourcing. `MarketDataSink` takes book/trade pushes;
`MarketDataSource` answers `top_of_book()` and `volume_since()` — the latter is
what POV sizes against.

## Routing & health

"Healthy" is three independent signals, each with its own trip and hysteresis:

| Signal | Trip | Recover |
|---|---|---|
| Connectivity | disconnected, or `now - last_seen > tolerance` | on reconnect + fresh data |
| Rejection rate | rate > high over a rolling window (false cancel-rejects excluded) | below low |
| Ack latency | p99 > high over a rolling window | below low |

Health updates from live events, not a poll loop; a timer only notices "nothing
arrived," which needs a clock. State machine: Healthy -> Degraded -> Down ->
(dwell) -> Probing -> Healthy. A venue trips at the high threshold and only
recovers below the low one after a dwell, so a borderline venue doesn't flap and
thrash routing. On recovery it comes back at reduced weight and must pass N clean
round trips before full weight — the venue that just came back is the least
proven.

`Router::select` picks the healthiest tradeable venue, or nothing. All-down: stop
submitting, cancel working orders, mark the cycle incomplete, alert. I don't
buffer — an order released after recovery trades a market that has already moved.

## Execution algos

`ExecutionAlgo` is a small event interface (`start`, `on_timer`, `on_fill`,
`on_reject`, `on_cancel`, `cancel`, `is_done`, `stats`). What keeps TWAP and POV
interchangeable: an algo stamps its own index into a scratch field on each child
order and filters the global event stream by that tag. The OMS needs no algo
registry and holds no algo references; an `AlgoDispatcher` does the routing.
Swapping algos is a factory string.

- **TWAP** splits the window into N slices and sizes each
  `ceil(remaining / remaining_slices)`, so partial fills are absorbed
  automatically and a zero slice is skipped rather than sent.
- **POV** sends `rate * volume_observed` per interval. When volume dries up it
  falls back in three stages: idle below a floor; if the shortfall grows past a
  threshold, degrade to a held TWAP over the residual window; at the deadline,
  cancel and cross with an IOC, else mark the parent incomplete.

When to prefer which: TWAP when finishing on schedule matters; POV when not being
a visible fraction of volume matters and I can tolerate under-filling in thin
markets.

## Signals & staleness

`SignalMessage { schema_version, sequence, generated_at, strategy_id, targets[] }`
over an in-process queue, with a length-prefixed binary codec that rejects
truncated buffers. Staleness is three checks because signals fail three ways:

| Check | Rule | Action |
|---|---|---|
| Age | `now - generated_at > max_age` | reject |
| Sequence | wrap-aware gap vs expected | forward gap: log and continue; backward: drop |
| Heartbeat | nothing within `interval * threshold` | producer presumed dead: stop new work, cancel resting |

Absurd/NaN targets are rejected at ingest, so a poisoned value never reaches the
delta.

## Netting

`NettingEngine` collapses per-strategy deltas into one net order per instrument,
holding the invariant `global == sum of sub-positions`. Fills attribute back
pro-rata by contribution, with the rounding remainder going to the largest
contributor (deterministic tie-break). Zero net delta: send nothing, but still
move the attributed sub-positions to their targets (an internal transfer) and
record the notional saved — "no order" still needs a bookkeeping update.

## Reconciliation

Source of truth: the exchange for what was executed, local state for intent.

Startup seeds from persisted state, fetches live positions, and stays in
do-not-trade until they reconcile — `SignalGate` boots rejecting every signal
until `enable_trading(can_start)` clears it.

| Condition | Action |
|---|---|
| Match | proceed |
| No persisted state (cold start) | adopt exchange, log |
| Drift <= threshold | adopt exchange (auto-heal), log |
| Drift > threshold | hard stop |
| Exchange unreachable | hard stop |

Steady state: `PeriodicReconciler` runs on its own timer, off the execution path.
It compares the exchange to the OMS filled-only position (in-flight excluded, or
every open order looks like drift), requires the drift to persist across cycles
before acting (so a fill in flight during the slow `get_positions()` isn't
chased), emits a `reconciliation`-tagged corrective order inside the auto-heal
band, and sticky-halts beyond it. Corrective orders are excluded from metrics and
netting.

## Failure recovery

Explicit policy per mode; one test each in `TestFailureModes.cpp`.

| # | Mode | Handling |
|---|---|---|
| 1 | No confirmation | ack timer marks the order, attempts a cancel, tells health; order stays live (never assumed dead) |
| 2 | Fill and cancel-ack out of order | fill wins; the cancel-reject is a false cancel-reject, excluded from health |
| 3 | Unsolicited cancel | applied and logged; the algo requeues the residual |
| 4 | Fill for unknown id | not applied; request a reconcile; hard-stop if it recurs |
| 5 | Duplicate fill | dropped via a per-order trade-id set |
| 6 | Stale fill after slot recycle | rejected by the generation bits |
| 7 | Disconnect mid-flight | keep in-flight orders; reconcile on reconnect |
| 8 | Reject storm | health trips, venue demoted, all-down if every venue trips |
| 9 | Stale or absent signal | age + sequence + heartbeat checks |
| 10 | Pool exhaustion | hard stop with a diagnostic |

Deferred on purpose (out of an 8-hour scope): exchange rate-limit backoff,
multi-leg atomicity, corporate actions, cross-venue aggregation, and journal
recovery after disk corruption.

## Metrics

`MetricsCollector` listens to the OMS event stream and records samples cheaply;
percentiles are computed at report time from a bounded ring, never an unbounded
vector.

- **Slippage** against **arrival mid** (mid when the OMS accepted the parent) —
  that captures the delay cost the execution layer owns. Also reported against
  VWAP, which forgives a slow start and isolates algo skill. Reporting one without
  the other hides where the cost came from.
- Fill rate, rebalance completion rate, time to fill, ack latency.
- Rejections, ack timeouts, unsolicited cancels, out-of-sequence drops, false
  cancel-rejects, demotions/recoveries, netting notional saved.

p50/p99/p99.9 per distribution. Honest caveat: with few samples p99.9 sits on a
single observation, so it's there for shape, not precision. `oms_bench` writes
`results/` — CSVs plus a REPORT.md that interprets the numbers.

## Concurrency

Covered in decision 1: one writer per loop, the client calls the OMS inline from
`poll()`, strategies react on the loop thread and run any heavy work on their own
loop, handing off at the boundary. Scale out by running one loop per venue/shard,
shared-nothing.

## Assumptions

- Market data is available; consumption is top-of-book + interval volume.
- Small instrument universe (tens to low hundreds), one loop.
- No multi-leg atomicity or corporate actions; fees are a simple field, not a schedule.
- Producer and OMS clocks are close enough for the age check; sequence and
  heartbeat catch the rest.
- Signal delivery is at-least-once and may reorder/duplicate; the gate makes it
  effectively exactly-once.

## What I'd do next

1. Real persistence — the fill journal + snapshot startup reconciliation assumes
   (today local state is passed in).
2. Live market data into POV and slippage, so arrival- and VWAP-relative slippage
   diverge as they would in a moving market.
3. A multi-venue benchmark so demotion shows up as a routing shift, not just
   all-down.
4. Amend-in-place order flow to cut cancel/replace churn.
5. Property-based tests for the netting invariant and the id codec.

## Scaling 5 -> 50 strategies

Netting breaks first: a net delta can't be computed until every strategy for that
instrument has published, so the slowest (or a stalled) producer sets the cycle
time. Then pro-rata rounding residuals accumulate into per-strategy position error
over a day. Then per-cycle work grows with strategies times overlapping
instruments.

What I'd change: shard event loops by instrument (one owner per instrument keeps
the single-writer property), make netting windowed instead of a hard barrier, and
attribute fills through an exact integer ledger instead of pro-rata rounding. The
trade-off is real: sharding by instrument breaks cross-instrument netting, because
one strategy's basket can span shards — you either add a cross-shard coordinator
(a barrier again) or accept per-instrument netting. I'd take per-instrument
netting and revisit only if un-netted crosses actually showed up in the slippage
numbers.
