#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "oms/ExchangeClient.h"   // pulls Types.h

namespace oms {

// Config for an exchange factory; venue-specific knobs are set on the concrete
// type in tests.
struct ExchangeConfig {
  VenueId  venue_id{0};
  uint64_t seed{0};
};

using ExchangeFactoryFn =
    std::function<std::unique_ptr<ExchangeClient>(const ExchangeConfig&)>;

// Owns exchange clients indexed by VenueId (spec 2.4); the Router selects among
// them. The name->constructor factory stands in for a dlopen plugin loader.
class ExchangeRegistry {
 public:
  void add(std::unique_ptr<ExchangeClient> client);

  ExchangeClient* get(VenueId v) const;
  std::vector<VenueId> venues() const;
  std::size_t size() const { return count_; }

  // ---- name -> constructor factory (no dynamic loading) ----
  static void register_factory(const std::string& name, ExchangeFactoryFn fn);
  static std::unique_ptr<ExchangeClient> create(const std::string& name,
                                                const ExchangeConfig& cfg);

 private:
  std::vector<std::unique_ptr<ExchangeClient>> by_venue_;   // index == venue id
  std::size_t count_{0};
};

}  // namespace oms
