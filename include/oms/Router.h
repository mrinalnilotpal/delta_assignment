#pragma once

#include <optional>
#include <vector>

#include "oms/Health.h"   // pulls Types.h

namespace oms {

// Routing sits behind an interface; the OMS depends only on select().
class Router {
 public:
  virtual ~Router() = default;
  // Choose a venue for this order, or nullopt if none is tradeable (all-down).
  virtual std::optional<VenueId> select(InstrumentId, Side, Quantity) = 0;
};

// Routes to the healthiest tradeable venue at submission time. Health updates
// from live events (via the HealthModel), never from a polling loop here.
class HealthAwareRouter : public Router {
 public:
  HealthAwareRouter(const HealthModel& health, std::vector<VenueId> venues)
      : health_(health), venues_(std::move(venues)) {}

  std::optional<VenueId> select(InstrumentId, Side, Quantity) override;

 private:
  const HealthModel&   health_;
  std::vector<VenueId> venues_;
};

}  // namespace oms
