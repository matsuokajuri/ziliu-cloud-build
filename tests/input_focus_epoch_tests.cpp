#include "ziliu/core/input_focus_epoch.h"

#include <cstdlib>
#include <limits>

namespace {
void Check(bool value) { if (!value) std::abort(); }
}

int main() {
  ziliu::core::InputFocusEpoch epoch;
  const auto continuous = epoch.Capture();
  // Ordinary consecutive keys preserve their owner revision.
  for (int key = 0; key < 30; ++key) Check(epoch.IsCurrent(continuous));
  epoch.Invalidate();
  const auto away = epoch.Capture();
  epoch.Invalidate();
  // A-B-A with identical context/text/selection still rejects A's old work.
  Check(!epoch.IsCurrent(continuous));
  Check(!epoch.IsCurrent(away));
  Check(epoch.IsCurrent(epoch.Capture()));
  const auto before_scope_query = epoch.Capture();
  epoch.Invalidate();
  Check(!epoch.IsCurrent(before_scope_query));
  const auto before_response = epoch.Capture();
  epoch.Invalidate();
  Check(!epoch.IsCurrent(before_response));
  const auto before_selection_query = epoch.Capture();
  epoch.Invalidate();
  Check(!epoch.IsCurrent(before_selection_query));
  Check(!epoch.IsCurrent(0));
  Check(ziliu::core::InputFocusEpoch::Next(std::numeric_limits<std::uint64_t>::max()) == 0);
  Check(ziliu::core::InputFocusEpoch::Next(0) == 0);
}
