# Execution Metrics Report

Deterministic run: seed 42, 5 rebalance cycles across 4 instruments, TWAP on {10,11} and POV on {12,13}, with injected partial fills, rejects, duplicate fills, and unsolicited cancels. All numbers are reproducible.

## Headline numbers

| Metric | p50 | p99 | p99.9 | count |
|---|---|---|---|---|
| Slippage arrival (bps) | 1 | 1 | 1 | 99 |
| Slippage VWAP (bps) | 1 | 1 | 1 | 99 |
| Time to fill (ns) | 4000000 | 4000000 | 4000000 | 99 |
| Ack latency (ns) | 1000000 | 1000000 | 1000000 | 99 |
| Fill rate (bps of target) | 10000 | 10000 | 10000 | 5 |

## Counters

- Filled orders: 99
- Rebalance completion rate: 1.000 (5/5)
- Rejections: 4
- Ack timeouts: 0
- Unsolicited cancels: 0
- Out-of-sequence events dropped: 10
- False cancel-rejects (excluded from health): 0
- Venue demotions: 0
- Netting saved notional: 2500000

## Interpretation

- **Slippage reference.** We report slippage against the **arrival mid** (mid at the instant the OMS accepted the parent), because that captures the full cost of the delay the execution layer is responsible for. We also report VWAP-relative slippage, which isolates algo skill from market drift. In this run the book is static, so the two coincide; in a moving market arrival-relative would exceed VWAP-relative whenever the market trended against us during the window. Reporting only one hides where the cost came from.
- **What p99 tells you.** p50 is the typical child order; p99 is the tail that sets risk limits and SLAs. Here the ack-latency p99 of 1000000ns reflects the sim's fixed ack latency; a real venue's p99 blowing out is the early warning of the reject/latency circuit breaker about to trip.
- **Why POV's fill rate differs from TWAP's.** TWAP sizes off the clock and will cross at the deadline, so it completes the target whenever liquidity is present. POV sizes off observed volume and deliberately *idles* below the volume floor, so when volume is thin POV under-fills within the window and only its deadline IOC closes the gap. POV trades completion certainty for footprint control.
- **What the demotion count implies.** 0 demotion(s): with a single venue, any demotion means the all-down policy engaged and a cycle was marked incomplete. With multiple venues it would instead show routing shifting away from the sick venue.
- **What I would alert on in production.** Rebalance completion rate below target and a rising reject/ack-latency p99 — those lead the circuit breaker. Raw fill counts are lagging indicators.
- **Honest limitation.** Distributions use a fixed-size ring reservoir (last N samples), so with few observations p99.9 collapses onto a single sample and is not statistically meaningful here; it is reported for shape, not precision.
