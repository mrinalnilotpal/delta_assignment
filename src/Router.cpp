#include "oms/Router.h"

namespace oms {

std::optional<VenueId> HealthAwareRouter::select(InstrumentId, Side, Quantity) {
  std::optional<VenueId> best;
  double best_weight = 0.0;

  for (VenueId v : venues_) {
    if (!health_.is_tradeable(v)) continue;
    const double w = health_.score(v).weight;
    // Strictly-greater keeps the lowest venue id on ties (stable routing).
    if (!best.has_value() || w > best_weight) {
      best = v;
      best_weight = w;
    }
  }
  return best;  // nullopt => all-down; caller enforces the all-down policy
}

}  // namespace oms
