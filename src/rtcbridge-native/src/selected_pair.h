#pragma once
#include "core.h"
#include <string>
#include <string_view>

namespace gamebridge::rtc {

// Internal identity only; never serialize these upstream identifiers.
struct SelectedPairIdentity {
  std::string pair, local, remote, local_type, remote_type;
  bool operator==(const SelectedPairIdentity &) const = default;
};

struct SelectedPairObservation {
  DirectPairEvidence evidence;
  bool retained_routine_probe{};
};

// Owned by the signaling thread. Invalid evidence forgets the old proof, so
// routine probes cannot establish initial proof or resurrect regressed proof.
class SelectedPairTracker {
public:
  void Invalidate() { proven_.reset(); }

  SelectedPairObservation Observe(const SelectedPairIdentity &identity,
                                  std::string_view state) {
    const auto reject = [&](DirectPairEvidence evidence) {
      Invalidate();
      return SelectedPairObservation{evidence, false};
    };
    if (identity.local_type == "relay" || identity.remote_type == "relay")
      return reject(DirectPairEvidence::Relay);
    if (identity.pair.empty() || identity.local.empty() || identity.remote.empty())
      return reject(DirectPairEvidence::Mismatched);
    const auto direct = [](std::string_view type) {
      return type == "host" || type == "srflx" || type == "prflx";
    };
    if (!direct(identity.local_type) || !direct(identity.remote_type))
      return reject(DirectPairEvidence::Unconvertible);
    if (state == "succeeded") {
      proven_ = identity;
      return {DirectPairEvidence::Direct, false};
    }
    // Pinned libwebrtc marks the already selected pair in-progress while a
    // routine STUN ping is outstanding. This is not proof of a new route.
    if (state == "in-progress" && proven_ && *proven_ == identity)
      return {DirectPairEvidence::Direct, true};
    return reject(DirectPairEvidence::Mismatched);
  }

private:
  std::optional<SelectedPairIdentity> proven_;
};
}
