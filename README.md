# Order Management & Execution System (OMS/EMS)

A production-quality execution layer for a market-neutral systematic book.
Research publishes target positions `{instrument → target_quantity}`; this
system computes deltas against live positions and works them into fills.

> **Status:** complete — domain types, order model, packed order-ID encoding,
> preallocated pool, **OrderManager** (full lifecycle + out-of-sequence +
> failure-mode handling), **ExchangeClient** interface + registry +
> **deterministic simulated exchange**, **market-data** interfaces,
> **circuit-breaker health model + health-aware router**, **execution algos
> (TWAP + POV with TWAP fallback)**, **signal transport + 3-way staleness**,
> **order netting**, **startup + periodic reconciliation**, **execution
> metrics**, and a **benchmark driver** that emits `results/`.

## Build & Run

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure   # 78 tests, all green, -Werror
./build/sim/oms_sim                            # minimal end-to-end demo
./build/sim/oms_bench                          # regenerates results/ (run from repo root)
```

- Language: **C++20**.
- Build: CMake, warnings-as-errors (`-Wall -Wextra -Werror`) on our targets.
- Tests: **Catch2 v3**, fetched via `FetchContent`. No external
  trading/messaging frameworks — standard library plus a test framework only.

## Project Layout

```
include/oms/   # headers, one per component (PascalCase Name.h)
  Types.h            core scalar domain types (integer-tick prices, byte enums)
  OrderId.h          packed 64-bit internal order id (encode/decode)
  Order.h            OrderStatus + Order struct (cache-conscious layout)
  OrderPool.h        fixed-capacity pool: freelist + per-slot generation
  Position.h         per-instrument net position (atomic apply)
  MarketData.h       TopOfBook/Trade + MarketDataSink/Source (2.5)
  ExchangeClient.h   ExchangeClient interface + ExchangeEventSink (2.4)
  ExchangeRegistry.h venue registry + name->constructor factory (2.4)
  OrderManager.h     lifecycle, 5-way validate, atomic positions (2.3)
  Health.h           circuit-breaker HealthModel (2.6)
  Router.h           health-aware Router + all-down (2.6)
  ExecutionAlgo.h    ExecutionAlgo + TWAP/POV + factory + dispatcher (2.7)
  Signal.h           SignalMessage/transport + 3-way staleness gate (2.8)
  Netting.h          NettingEngine: net delta + pro-rata attribution (2.9)
  Reconciliation.h   startup + periodic reconciliation (2.10)
  Metrics.h          MetricsCollector + bounded Distribution (2.12)
  RingBuffer.h       bounded rolling-window ring buffer
  Logging.h          structured log sinks
src/           # implementations (Name.cpp)
sim/           # simulated exchange (deterministic, seeded); oms_sim + oms_bench
tests/         # Catch2 unit tests, organised by behaviour (TestName.cpp)
results/       # benchmark output (committed): CSVs + REPORT.md
```

---

## Design Principles

These shape everything else and are non-negotiable in the reference system.

1. **Single-writer event loop, no locks on the order path.** One thread reads
   the transport, parses it, mutates order + position state, and invokes
   strategy callbacks — all on one synchronous call stack. Concurrency comes
   from *independent* event loops, never from locking shared order state. A fill
   must update the order and the position atomically-by-construction; if they
   interleave, the execution delta silently corrupts.
2. **Fills win over cancels; late events are dropped, never applied.** If a fill
   arrives while a cancel is in flight, the fill is applied. Any event for an
   order already in a terminal state is logged as an anomaly and discarded.
3. **Terminal states are the dedup boundary.** Exactly three: `Cancelled`,
   `Filled` (COMPLETED), `Rejected`. Once terminal, the order leaves the live
   index and no further event can mutate it — this single rule kills most
   double-counting bugs.
4. **Pending-cancel / pending-modify are flags orthogonal to status, not
   statuses.** A status alone cannot express "partially filled *and* has a
   cancel in flight", so in-flight requests live in separate flags.
5. **Health is a binary, auto-resolving staleness state; risk halts are sticky
   and manual.** Transient venue/data problems self-heal; risk trips (position,
   PnL, unacked-order limits) require a human to re-arm.
6. **Preallocate everything on the hot path.** Orders come from a fixed-size
   pool with a freelist. No allocation between signal and wire.
7. **The system depends only on interfaces.** The order manager knows
   `ExchangeClient`, not "Binance"; the router knows a health model, not a
   rejection counter; algos know the order manager, not each other. The
   simulated exchange implements the *same* interface as a real one.

## Core Domain Types (`Types.h`)

- **Prices are integer ticks** (`int64_t`), never `double`: binary floating
  point cannot represent a tick grid exactly, and accumulated fill amounts must
  reconcile to the cent. Doubles appear only at the logging boundary.
- **Quantities are signed** (`int64_t`); the sign carries direction for targets
  and deltas.
- **Enums are byte-width** (`uint8_t`) so they pack into the hot order layout.

## Order Model (`Order.h`)

- `OrderStatus` is a single byte; `is_terminal()` marks the three terminal
  states (principle 3).
- **Cancel/fill race handling (highest-signal detail):** `pending_cancel`
  mirrors the reference `_outstandingSC`. A fill may drive status to
  `PartiallyFilled`/`Filled` while a cancel is still outstanding; when that
  happens the incoming cancel-reject is an *artifact of the fill*, not a real
  error, so we mark `false_cancel_reject`. **The health model must exclude
  false cancel-rejects from its rejection rate.**
- **Average price without float drift:** we accumulate signed notional in a
  128-bit `total_fill_amount` and divide on demand (`avg_fill_price()`), never
  accumulating a floating average incrementally.
- **Single source of truth:** `remaining() = size − filled_size` is computed,
  never stored, so it cannot drift.
- **Cache-conscious layout:** the fields touched on every ack/fill are grouped
  into a `do-not-reorder` hot block at the front of an `alignas(64)` struct.

## Internal Order ID Encoding (`OrderId.h`)

The brief requires routing inbound events back to the owning order **without a
hash lookup**. The id is a packed 64-bit integer decoded by arithmetic:

```
 63          56 55        40 39                    16 15             0
+-------------+------------+------------------------+----------------+
|  venue_id   | generation |       slot_index       |    sequence    |
|   8 bits    |  16 bits   |        24 bits         |    16 bits     |
+-------------+------------+------------------------+----------------+
```

- **`venue_id`** — which `ExchangeClient` the order was routed to; an inbound
  event dispatches to the right book with no lookup.
- **`slot_index`** — direct index into the preallocated pool: `pool[slot]` is
  O(1), no hash.
- **`generation`** — bumped every time a slot is recycled. **Safety mechanism:**
  a stale event referencing a recycled slot carries the old generation and is
  rejected (`OrderPool::is_current`), so a late fill for a dead order can never
  mutate the different, live order now occupying that slot.
- **`sequence`** — monotonic per venue, for ordering diagnostics and log
  correlation.

`encode_order_id` / `decode_order_id` are `constexpr` free functions.

## Order Pool (`OrderPool.h`)

Fixed-capacity pool (default 100,000) with a freelist and a per-slot generation
counter. All slots are constructed and their pages touched in the constructor so
the first order of the session does not take a page fault on the wire path.
Releasing a slot bumps its generation; **pool exhaustion is a hard stop
(assert), never a silent drop.**

---

## Architecture

```
  EXECUTION PATH (one single-writer event loop)          RECONCILE LOOP (own timer)
  ---------------------------------------------          ----------------------------
  signals --> SignalGate (2.8) --> NettingEngine (2.9)     PeriodicReconciler (2.10)
              +---------------------+                        - get_positions() (slow)
              |  Strategy / Algo    | TWAP/POV (2.7)         - drift = exch - oms(filled)
              +----------+----------+                        - band? corrective (tagged)
                         | submit(OrderRequest)              - beyond? sticky HALT
                         v                                          |
     +-------------------+--------------------+  select   +---------+----------+
     |             OrderManager               |--------->|      Router (2.6)   |
     |  pool, live index, positions, validate |          +---------+----------+
     |  ExchangeEventSink (on_confirm/fill/..)|  events         ^ is_tradeable/score
     +--+-----------+--------------+----------+  --------> HealthModel (2.6)
        | subscribe | place/cancel ^ poll() delivers events (same stack)
        v           v              |
  MetricsCollector  ExchangeRegistry (by VenueId) --> ExchangeClient <== SimulatedExchange
     (2.12)                                               ^  pull book/volume
                                                          +-- MarketDataSource (2.5)
```

The reconcile loop is drawn **beside** the execution path deliberately: it runs
on its own timer, only reads positions and (within the auto-heal band) submits
corrective orders — it never holds the order path and is never held by it
(demonstrated in `TestPeriodicRecon`). The OMS depends only on interfaces
(`ExchangeClient`, `Router`, `HealthModel`, `MarketDataSource`); the simulated
exchange implements the same `ExchangeClient` as a real venue would.

## Concurrency & async event delivery (2.4)

- **Single-writer event loop, no locks on the order path.** The exchange client
  calls the `ExchangeEventSink` (the `OrderManager`) **synchronously from within
  `poll()`**, on the thread that drained the transport. There is **no queue**
  between the exchange and the OMS.
- Because there is exactly one writer, the OMS is **lock-free by construction**.
  A fill mutates the order and the position on one call stack, so they can never
  be observed inconsistently.
- **What a strategy is forbidden from doing:** mutating OMS state from any other
  thread. Strategies react to published `OrderEvent`s on the loop thread; if they
  need their own work loop, they run an *independent* loop and hand off only at
  the boundary.
- **Scaling:** run one event loop per venue/shard, each single-writer, pinned to
  a core; shared-nothing between loops. (See the upcoming scaling section.)

## Order lifecycle & out-of-sequence handling (2.3)

- **Status** is a single byte with three terminal states (`Filled`,
  `Cancelled`, `Rejected`). `pending_cancel` is an **orthogonal flag**, not a
  status, so an order can be "partially filled *and* has a cancel in flight".
- **One shared guard sequence** — `validate(id, EventContext)` — runs at the top
  of every inbound handler and covers all five failure modes: (1) unknown order,
  (2) terminal state, (3) duplicate trade id, (4) cumulative/over-fill via the
  over-fill guard, (5) field sanity (positive price/qty). Anything but `Ok` is
  **logged with full context and returned** — never silently applied.
- **Fills win over cancels.** If a fill completes an order while a cancel is in
  flight, the fill is applied and the order goes terminal. The cancel-reject that
  follows is recognised as a **false cancel-reject** (`false_cancel_reject`,
  counted) and is **excluded from the health rejection rate**.
- **Kill switch.** A fill for an order we do not know about
  (`ORDER_FILLED_NOT_IN_SYSTEM`) means our position is already wrong — this is a
  sticky hard stop; further submits are refused.
- **Ordering invariant (atomicity).** Inside `on_fill` the position is updated
  **before** any subscriber runs, so no strategy can observe a fill before the
  position reflects it. Tested directly.
- **Deferred retirement.** Terminal orders leave the live index but their pool
  slot is freed only after a bounded grace window, so recent duplicate/late
  events resolve to `TerminalState` (dropped) rather than `UnknownOrder` (kill).
  Beyond the window the slot is reclaimed and its generation bumps, so genuinely
  stale events become unknown — no double counting, no false kills.

## Exchange client, registry & simulated exchange (2.4)

- `ExchangeClient` is a "fat" base: only `place_order` / `cancel_order` and a few
  queries are pure virtual; capability methods (e.g. `mass_cancel`) **default**
  to "not supported". `place_order`/`cancel_order` return a rich `SendResult`
  (`Ok`, `RejectedLocally`, `TransportDown`, `RateLimited`, `Duplicate`).
- `ExchangeRegistry` owns clients indexed by `VenueId`, with a
  `name -> constructor` factory (stands in for the reference's dlopen plugin).
- `SimulatedExchange` implements the same interface with **deterministic, seeded**
  control over ack/fill latency, partial-fill chunking, reject injection,
  unsolicited cancels, event **reordering** (fill before ack), **duplicate**
  events, and disconnects. Logical time advances per `poll()`, so every run is
  reproducible; the seed is a parameter.

## Market data consumption (2.5)

Push-primary, pull-secondary: `MarketDataSink` receives book/trade updates;
`MarketDataSource` answers `top_of_book()` and `volume_since()` (what POV will
consume). **Prices/volumes are assumed available — we model consumption, not
sourcing.**

## Exchange health & routing (2.6)

Health is a composite of **three independent signals**, each with its own trip
and hysteresis:

| Signal | Measure | Trip | Recovery |
|---|---|---|---|
| Connectivity | `is_connected()` + heartbeat staleness | disconnected, or `now − last_seen > tolerance` | auto, on reconnect + freshness |
| Rejection rate | rejects/sends over a rolling window, **excluding false cancel-rejects** | rate > `reject_high` with min samples | auto, below `reject_low` |
| Ack latency | p99 of ack round-trip over a rolling window | p99 > `latency_high` | auto, below `latency_low` |

- **Event-driven, not polled.** Counters update inside `on_ack`/`on_reject`/etc.
  A timer (`tick`) is used **only** to evaluate time-based staleness — you cannot
  detect "nothing arrived" without a clock. Threshold arithmetic stays off the
  hot path.
- **Hysteresis + state machine:** `Healthy → Degraded → Down → (dwell) →
  Probing → Healthy`. Trip at `T_high`, recover only below `T_low` after a dwell,
  so a venue at the boundary does not flap and thrash routing.
- **Probing on recovery:** a recovered venue re-enters at **reduced weight** and
  must complete N successful round trips before full restoration — a venue that
  just came back is the least proven.
- **Routing:** `Router::select` returns the healthiest *tradeable* venue (highest
  weight; `Down` = not tradeable), or `nullopt` when none are.
- **All-down policy:** when no venue is tradeable the OMS **stops submitting,
  does not buffer, cancels working orders where the transport permits, marks the
  rebalance cycle incomplete, and alerts.** A buffered order released after
  recovery would execute against a market that has moved — so we never buffer.
- Rolling windows are fixed-size ring buffers (bounded memory/cost).

## Rolling windows

`RingBuffer<T>` is a fixed-capacity ring; the rejection-rate and latency signals
each keep the last N samples so old data ages out automatically.

---

## Execution algorithms (2.7)

`ExecutionAlgo` is a thin event-listener interface (`start`, `on_timer`,
`on_fill`, `on_reject`, `on_cancel`, `cancel`, `is_done`, `stats`). TWAP and POV
both implement it; **switching between them requires no OrderManager changes.**

- **What keeps them interchangeable — the routing tag rides on the order.** An
  algo stamps its own index into a scratch field on each child order it sends
  (`Order::store_int_data(index, offset)`). It then filters the *global*
  order-event stream: `if (order.retrieve_int_data(offset) == my_index) …`. The
  OMS needs **no algo registry, no per-algo subscription bookkeeping, and no
  ownership map** — the tag survives everything and cannot get out of sync. An
  `AlgoDispatcher` (an `OrderEventListener`, *not* part of the OMS) performs the
  match and calls the owning algo's typed `on_fill`/`on_reject`/`on_cancel`.
- **Self-check:** the OrderManager contains **zero** references to `TwapAlgo` or
  `PovAlgo` — `grep -R "TwapAlgo\|PovAlgo" src/OrderManager.cpp include/oms/OrderManager.h`
  returns nothing. Algos are named only by `AlgoFactory::create("twap"|"pov", cfg)`.
- **TWAP.** Divide `[start, end]` into `N` slices. Each boundary sizes
  `slice_qty = ceil(remaining / remaining_slices)`, where `remaining` is
  recomputed as `target − filled − in-flight`, so **prior partial fills are
  absorbed automatically** with no special-casing. The final slice sends all
  remaining; a zero slice (over-filled / lot-rounded) is **skipped, not sent**.
- **POV.** Each interval sizes `child = rate × volume_observed`, where volume
  comes from `MarketDataSource::volume_since`. Cumulative participation is
  tracked so `filled / market_volume ≈ rate` (asserted in a test).
- **POV fallback (three stages), when volume dries up:**
  1. **Below a volume floor:** do nothing, record an `idle_interval` (never send a
     0- or round-up-to-1 order).
  2. **Shortfall beyond a catch-up threshold:** the participation constraint has
     become unsatisfiable, so degrade to the *time* constraint — **POV holds a
     `TwapAlgo` instance** and delegates the residual window to it. This is the
     payoff of the shared interface.
  3. **At the deadline:** cancel resting, cross the spread with an **IOC** for the
     residual, and if still unfilled mark the parent `incomplete_at_deadline` and
     alert. **Never a silent residual.**
- **Unified deadline behaviour (worth calling out).** The reference had one
  worker cross the spread at the deadline and another that only cancelled. We
  **unified** it: both TWAP and POV cancel resting *and* send an IOC across the
  spread at the deadline. Consistency beats a per-worker surprise.

## Signal transport & staleness (2.8)

`SignalMessage { schema_version, sequence, generated_at, strategy_id,
vector<TargetPosition> }`. Transport is an in-process queue
(`InProcessSignalQueue`); a length-prefixed **binary codec**
(`serialize_signal` / `deserialize_signal`) is the file/stdin adapter and
rejects truncated/garbage buffers.

Staleness is detected at the **transport layer** with **three independent checks
because they fail differently**:

| Check | Rule | Action |
|---|---|---|
| **Age** | `now − generated_at > max_signal_age` | **reject** the signal, do not act |
| **Sequence gap** | wrap-aware `miss = (seq − expected + MOD) % MOD`, threshold-based | forward gap → **log & continue** (next full snapshot heals it); backward/near-wrap → **duplicate/reorder, dropped** |
| **Heartbeat** | no signal within `hb_interval × hb_threshold` | producer presumed dead → **stop new work, cancel resting** (a stale target is worse than no target) |

NaN/absurd target values are **rejected at ingest** (strengthened from the
reference, which only logged) so they never enter the delta computation. The
brief's delta itself is `OrderManager::execution_delta(inst, target) = target −
(position + pending)`.

## Order netting (2.9)

`NettingEngine` collects per-strategy deltas within a rebalance cycle and emits
**one net order per instrument**:
`net_delta[inst] = Σ_s (target[s][inst] − attributed[s][inst])`.

- **Invariant:** `global_position == Σ sub_positions`, maintained on every fill
  and seeding, and `check_invariant()` asserts it (tested).
- **Attribution rule (documented, deterministic):** each fill is attributed
  pro-rata by a strategy's contribution to the net delta; the rounding residual
  is assigned by **largest-remainder to the largest contributor** (tie-break:
  larger weight, then smaller strategy id).
- **Zero net delta (the case the brief asks about):** when `net_delta == 0` but
  individual strategy deltas are non-zero, **send nothing** — but the internal
  transfer is still a **book entry**: attributed positions move to their targets
  and `netting_saved_notional += Σ|delta_s| × price / 2`. "No order" still
  requires a position bookkeeping update.
- Every netting decision is logged with the per-strategy inputs for offline audit.

## Positions & startup reconciliation (2.10)

- **Position model.** `Position { net, overnight, volume, amount, fees,
  last_update }`. We track **net quantity only**; long/short leg splitting is a
  **venue-rules concern** (China open/close priority, Hong Kong short-sell
  marking), not a sensible global default, so it is deliberately out of the
  default model.
- **Source-of-truth policy.** Positions seed from a **persisted local file**; the
  exchange is authoritative for *executed* reality; open orders are downloaded
  from the exchange at boot. Trading stays in **do-not-trade (DNT)** mode until
  reconciliation passes. We **hard-stop rather than auto-heal** on a large boot
  discrepancy: at startup a human is present and unlimited time exists, and
  guessing can double the day's first risk.

| Condition | Action | Why |
|---|---|---|
| Match | **proceed** | — |
| No persisted state (cold start) | **adopt exchange**, log loudly | exchange is the only truth available |
| Drift ≤ `startup_auto_heal_threshold` | **adopt exchange**, log, emit metric | small drift = a fill missed while down; exchange is authoritative for executed reality |
| Drift > threshold | **HARD STOP**, non-zero exit, alert | cannot distinguish missed fill from corruption; a human must decide |
| Exchange unreachable | **HARD STOP** | cannot verify; trading blind is worse than not trading |

`StartupReconciler::reconcile(...)` returns a per-instrument decision table plus
`can_start` (gates the DNT→trading transition) and the `adopted` seed positions.
**The gate is wired:** `SignalGate` **boots in do-not-trade** and returns
`RejectDNT` for every signal until `enable_trading(report.can_start)` clears it —
so no signal is acted on before reconciliation completes (tested).

### Periodic (steady-state) reconciliation

`PeriodicReconciler` runs **independently of the execution path** (its own timer):

```
every reconcile_interval:
  exchange_pos = Σ venue.get_positions()      # may be slow; runs off the exec path
  for each instrument:
    drift = exchange_pos - oms_position(FILLED ONLY)   # in-flight EXCLUDED
    if |drift| <= reconcile_threshold:  continue        # logged [[RESOLVED]]
    if drift persists < persist_cycles: continue        # confirm before acting
    if |drift| <= auto_heal_band:       corrective order (tagged) + metric
    else:                                sticky HALT + alert + manual re-arm
```

Non-obvious details handled (and why):

- **In-flight is excluded from the comparison.** We compare exchange positions to
  the OMS **filled-only** position (`position().net`); working quantity lives in
  `pending_quantity()` and is deliberately left out, or every open order would
  look like drift (tested in `TestPeriodicRecon`).
- **Snapshot-skew guard.** A drift must **persist across `persist_cycles`
  consecutive cycles** before we act, so a fill in flight during the (slow)
  `get_positions()` read is not chased.
- **Corrective orders are tagged `reconciliation`.** They are excluded from
  execution-quality metrics *and* from netting, so a housekeeping trade never
  pollutes slippage or nets against a strategy's real intent.
- **The halt is sticky and manual** (principle 5); `PeriodicReconciler::rearm()`
  is the re-entry point.
- **Diagnosable logs:** instrument, both positions, the delta, the reconcile
  result, and timestamps — enough to debug offline.
- **Source of truth:** the **exchange is the source of truth for executed
  reality; local state is the source of truth for intent.** That single sentence
  resolves every ambiguous case.

---

## Failure recovery (2.11)

Explicit handling per mode; **one test per row** (`TestFailureModes.cpp`, plus
lifecycle/algo tests). Policy, not case-by-case:

| # | Failure mode | Policy |
|---|---|---|
| 1 | Confirmation never arrives | Per-order **ack timer** (`check_ack_timeouts`). On expiry: mark `ack_timed_out`, attempt a cancel, feed `on_timeout` to health, and treat the order as **possibly live — never assume dead** (not retired). |
| 2 | Fill & cancel-ack out of order | Fill applied; cancel-reject for a now-terminal order is a **false cancel-reject**: logged, counted, **excluded from health**. |
| 3 | Unsolicited cancel | **Apply it** (exchange is authoritative), log as anomaly, feed health, count; the **algo requeues the residual** on its next timer — the parent target is unchanged. |
| 4 | Fill for unknown order id | Log at error, **do not apply**, **request an immediate reconcile**, and **hard-stop only if it recurs beyond a threshold** (`set_unknown_fill_hardstop_threshold`). One lost mapping is recoverable; a storm is corruption. |
| 5 | Duplicate fill (same trade id) | Per-order trade-id set; second sighting **dropped and counted**. |
| 6 | Stale fill after recycle | Caught by the **generation bits** in the order id (`is_current` fails) → treated as unknown; tested explicitly. |
| 7 | Disconnect mid-flight | Mark venue down (`on_venue_disconnect`); **keep in-flight orders** (not assumed dead). On reconnect (`on_venue_reconnect`) **request a reconcile** before trusting local state. |
| 8 | Reject storm | Health trips on **rejection rate**; venue demoted; **all-down policy** engages if every venue trips. |
| 9 | Stale/absent signal | **Age + sequence + heartbeat** checks (2.8); on producer death, cancel resting orders. |
| 10 | Pool exhaustion | **Hard stop with a diagnostic** (`ORDER_POOL_EXHAUSTED`), never a silent drop. |

**Halt vs auto-heal — the general principle:** auto-heal only when the safe action
is unambiguous and bounded (a small, explainable drift; a transient venue
problem). The moment we cannot distinguish "missed fill" from "corrupted state,"
we **halt and let a human decide** — guessing at boot or on a large drift can
double real risk.

**Deferred (named on purpose):** exchange-side rate-limit backoff, multi-leg
atomicity, corporate actions, cross-venue position aggregation, and cold-start of
the persisted journal after disk corruption. These are real but out of an 8-hour
scope; naming them is part of the honesty.

---

## Execution metrics (2.12)

`MetricsCollector` subscribes to the OMS event stream and **records rich samples
cheaply on the hot path**; percentiles are computed **off it** at report time.
Distributions land in a **fixed-size ring reservoir** (`Distribution`, last N
samples) — never an unbounded vector — so cost and memory are bounded.

- **Slippage.** `(avg_fill_price − arrival_mid) × side_sign`, in **bps**, where
  **arrival mid is the mid at the instant the OMS accepted the parent**. We chose
  arrival mid because it captures the **full cost of the delay the execution
  layer is responsible for** (decision-to-fill). We **also** report
  **VWAP-relative** slippage, which forgives a slow start and isolates **algo
  skill** vs. market drift. Reporting one without the other hides where the cost
  came from.
- **Fill rate** (`filled/target` per cycle), **rebalance completion rate**
  (fraction of cycles fully filled), **time to fill** (submission→terminal,
  distribution), **ack latency** (submit→confirm, distribution).
- **Error/recovery rates:** rejections, ack timeouts, unsolicited cancels,
  out-of-sequence drops, false cancel-rejects, venue demotions — counts + rates.
- **Netting savings:** notional not traded due to netting.
- **p50 / p99 / p99.9** are reported for every distribution. **Honest
  limitation:** with few samples p99.9 collapses onto a single observation — it is
  reported for shape, not precision (stated in the report too).

**Deliverable — `results/`** (regenerate with `./build/sim/oms_bench`): a
deterministic multi-cycle run across 4 instruments with **both** algos and
injected failures, emitted as `metrics_summary.csv` + `cycles.csv` + a
**`REPORT.md` that interprets** the numbers (what p99 means, why POV's fill rate
differs from TWAP's, what the demotion count implies, and which metric to alert
on).

---

## Test suite (2.13)

Organised by **behaviour**, every test runs against the deterministic seeded
`SimulatedExchange`. Coverage: order lifecycle (happy path, partial→complete,
average price, reject-after-send, cancel accepted, **cancel/fill race**,
unsolicited cancel, duplicate trade id, terminal-order event, **unknown order**,
**stale recycled-slot event**), order-id encode/decode round trip, the **ten
failure modes** (one test per row), routing/health (scoring, demotion,
hysteresis, false-cancel-reject exclusion, all-down), execution algos (TWAP
slices sum to target, **canonical buy-100-over-10 with a slice-3 partial and no
over-send**, deadline behaviour, POV sizing/idle/degrade/cumulative rate,
swappability), netting (opposing→zero, partial offset, invariant, residual),
startup + periodic reconciliation (drift bands, in-flight exclusion, loop
independence), and signals (age, gap, wrap, duplicate, heartbeat, NaN).

---

## Assumptions

- **Market data is available**; we model consumption (`top_of_book`,
  `volume_since`), not sourcing. Granularity is top-of-book + interval volume.
- **Instrument universe is small** (tens–low hundreds) and fits one loop.
- **No multi-leg atomicity, no corporate actions**, and a **simplified fee model**
  (fees plumbed on `Position`, not a slab calculator).
- **Clock sync** between producer and OMS is good enough that age checks are
  meaningful; sequence + heartbeat catch the rest.
- **Signal delivery** is at-least-once and may reorder/duplicate; the gate makes
  it effectively exactly-once by intent.
- Logical time is the sim's `poll()` clock, so **every run is reproducible**.

## What I'd do next (prioritised)

1. **Persistence:** the write-through fill journal + periodic snapshot that
   startup reconciliation assumes (currently the local state is passed in).
2. **Real market-data plumbing** into POV/slippage (live VWAP), so
   VWAP-relative slippage diverges from arrival-relative as it would in a moving
   market.
3. **Multi-venue benchmark** so demotion/recovery shows as *routing shift*, not
   just all-down.
4. **Modify/replace** order flow (amend in place) to reduce cancel/replace churn.
5. **Property-based tests** for the netting invariant and the id codec across
   boundary values.

## Bonus — scaling from 5 to 50 strategies

- **What breaks first: the netting cycle becomes a synchronisation barrier.** A
  net delta can only be computed once **every** strategy for that instrument has
  published, so the **slowest producer sets the cycle time and a stale producer
  stalls all 50**. The single-writer loop is fine on throughput; the barrier is
  the bottleneck.
- **Second: fill attribution gets ambiguous.** With 50 strategies contributing to
  one net order, pro-rata **rounding residuals accumulate** into meaningful
  per-strategy position error over a day.
- **Third: per-cycle netting work grows with strategies × instruments** as
  universes overlap.
- **What I'd change:** shard event loops **by instrument** (each instrument owned
  by exactly one loop, preserving single-writer per instrument); make netting
  **windowed/bounded** rather than barrier-synchronous (trade a little netting
  efficiency for latency determinism); move attribution to an **explicit ledger
  with exact integer allocation** instead of pro-rata rounding.
- **The trade-off, named:** **sharding by instrument preserves the lock-free
  invariant but breaks cross-instrument netting**, because two instruments in one
  strategy's basket may live on different shards. Resolving that means either a
  cross-shard netting coordinator (reintroducing a barrier) or accepting
  per-instrument netting only — a deliberate choice, not an accident.

---

## Part 3 — where this diverges from a "reference" production design

Being explicit about divergences (judgement is part of the evaluation):

- **NaN/absurd targets are rejected at ingest**, not merely logged. A poisoned
  target must never reach the delta computation.
- **Unified deadline behaviour across algos:** both TWAP and POV cancel resting
  *and* cross with an IOC at the deadline, rather than one worker crossing and
  another only cancelling. Consistency beats a per-worker surprise.
- **A periodic reconciler that can emit a corrective order** (within a tight
  band). A common production stance is *reconcile logs only, a separate
  liquidator flattens risk*. We implement the brief's corrective path but flag its
  tension honestly: **a corrective *trade* moves both the exchange and the OMS
  book by the same amount**, so it truly closes a drift only when the drift is an
  un-applied fill (a belief gap). We therefore keep the auto-heal band **tight**,
  tag the order so it never pollutes metrics/netting, and **hard-stop** beyond the
  band rather than trade our way into a bigger hole.
- **Slippage is computed online** (the reference defers it offline). It is cheap
  here and makes the `results/` report self-contained; the sampling is still
  bounded (ring reservoir), honouring "capture cheap, aggregate off the hot path."
