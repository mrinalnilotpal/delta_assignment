#include "oms/ExchangeRegistry.h"

#include "oms/Detail.h"

namespace oms {

void ExchangeRegistry::add(std::unique_ptr<ExchangeClient> client) {
  assert(client && "null exchange client");
  const VenueId v = client->venue_id();
  if (by_venue_.size() <= v) by_venue_.resize(static_cast<std::size_t>(v) + 1);
  assert(!by_venue_[v] && "venue id already registered");
  by_venue_[v] = std::move(client);
  ++count_;
}

ExchangeClient* ExchangeRegistry::get(VenueId v) const {
  if (v >= by_venue_.size()) return nullptr;
  return by_venue_[v].get();
}

std::vector<VenueId> ExchangeRegistry::venues() const {
  std::vector<VenueId> out;
  for (std::size_t i = 0; i < by_venue_.size(); ++i) {
    if (by_venue_[i]) out.push_back(static_cast<VenueId>(i));
  }
  return out;
}

void ExchangeRegistry::register_factory(const std::string& name, ExchangeFactoryFn fn) {
  detail::factory_table()[name] = std::move(fn);
}

std::unique_ptr<ExchangeClient> ExchangeRegistry::create(const std::string& name,
                                                         const ExchangeConfig& cfg) {
  auto it = detail::factory_table().find(name);
  if (it == detail::factory_table().end()) return nullptr;
  return it->second(cfg);
}

}  // namespace oms
