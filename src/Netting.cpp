#include "oms/Netting.h"

namespace oms {

std::vector<NettingEngine::NetOrder> NettingEngine::compute_cycle(
    const std::unordered_map<InstrumentId, Price>& prices) {
  std::vector<NetOrder> orders;

  // Deterministic instrument order for reproducible logs/attribution.
  std::vector<InstrumentId> insts(instruments_.begin(), instruments_.end());
  std::sort(insts.begin(), insts.end());
  std::vector<StrategyId> strats(strategies_.begin(), strategies_.end());
  std::sort(strats.begin(), strats.end());

  for (InstrumentId inst : insts) {
    std::vector<std::pair<StrategyId, Quantity>> contribs;   // (strategy, delta_s)
    Quantity net = 0;
    Quantity gross_abs = 0;
    for (StrategyId s : strats) {
      auto tit = targets_.find(key(s, inst));
      if (tit == targets_.end()) continue;               // no target this cycle -> no change
      const Quantity delta = tit->second - attributed(s, inst);
      if (delta == 0) continue;
      contribs.emplace_back(s, delta);
      net += delta;
      gross_abs += (delta < 0 ? -delta : delta);
    }

    if (contribs.empty()) continue;

    Price price = 0;
    if (auto pit = prices.find(inst); pit != prices.end()) price = pit->second;

    if (net == 0) {
      // Zero net delta: send NOTHING, but the internal transfer is still a book
      // entry -- attributed positions move to their targets, global is unchanged.
      saved_notional_ += static_cast<int64_t>(gross_abs) * static_cast<int64_t>(price) / 2;
      for (const auto& [s, delta] : contribs) attributed_[key(s, inst)] += delta;
      if (log_) {
        log_->log(LogLevel::Info,
                  "netting zero-net inst=" + std::to_string(inst) +
                      " strategies=" + std::to_string(contribs.size()) +
                      " saved_notional+=" +
                      std::to_string(static_cast<int64_t>(gross_abs) *
                                     static_cast<int64_t>(price) / 2));
      }
      continue;
    }

    contrib_[inst] = contribs;   // remember for pro-rata attribution of fills
    orders.push_back({inst, net});
    if (log_) {
      log_->log(LogLevel::Info, "netting order inst=" + std::to_string(inst) +
                                    " net=" + std::to_string(net) +
                                    " contributors=" + std::to_string(contribs.size()));
    }
  }
  return orders;
}

void NettingEngine::attribute_fill(InstrumentId inst, Quantity signed_qty, Price) {
  global_[inst] += signed_qty;

  auto it = contrib_.find(inst);
  if (it == contrib_.end() || it->second.empty()) {
    return;   // no recorded contributions; nothing to attribute
  }
  const auto& contribs = it->second;

  Quantity total_abs = 0;
  for (const auto& [s, d] : contribs) total_abs += (d < 0 ? -d : d);
  if (total_abs == 0) return;

  const Quantity sign = signed_qty < 0 ? -1 : 1;
  const Quantity absq = signed_qty < 0 ? -signed_qty : signed_qty;

  // Largest-remainder apportionment on absolute quantity.
  struct Alloc { StrategyId s; Quantity base; Quantity rem; Quantity weight; };
  std::vector<Alloc> allocs;
  allocs.reserve(contribs.size());
  Quantity distributed = 0;
  for (const auto& [s, d] : contribs) {
    const Quantity w = (d < 0 ? -d : d);
    const Quantity num = absq * w;
    const Quantity base = num / total_abs;
    const Quantity rem = num % total_abs;
    allocs.push_back({s, base, rem, w});
    distributed += base;
  }

  Quantity leftover = absq - distributed;
  // Assign leftover units largest-remainder first; tie-break by larger weight,
  // then smaller strategy id -- fully deterministic.
  std::sort(allocs.begin(), allocs.end(), [](const Alloc& a, const Alloc& b) {
    if (a.rem != b.rem) return a.rem > b.rem;
    if (a.weight != b.weight) return a.weight > b.weight;
    return a.s < b.s;
  });
  for (std::size_t i = 0; i < allocs.size() && leftover > 0; ++i, --leftover) {
    allocs[i].base += 1;
  }

  for (const auto& a : allocs) {
    attributed_[key(a.s, inst)] += sign * a.base;
  }
}

bool NettingEngine::check_invariant() const {
  for (InstrumentId inst : instruments_) {
    Quantity sum = 0;
    for (StrategyId s : strategies_) sum += attributed(s, inst);
    if (sum != global_position(inst)) return false;
  }
  return true;
}

}  // namespace oms
