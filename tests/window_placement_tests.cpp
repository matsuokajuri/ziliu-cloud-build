#include "ziliu/core/window_placement.h"

#include <cstdlib>

namespace {
void Check(bool value) { if (!value) std::abort(); }
}

int main() {
  using ziliu::core::FitWindowToWorkArea;
  using ziliu::core::WindowRectangle;
  // Actual 1280x720 guest: the taskbar leaves 672 pixels of usable height.
  Check(FitWindowToWorkArea({130, 130, 1100, 820}, {0, 0, 1280, 672}) ==
        WindowRectangle{130, 0, 1100, 672});
  // Preferred geometry remains unchanged when the entire frame fits.
  Check(FitWindowToWorkArea({130, 130, 1100, 820}, {0, 0, 1920, 1040}) ==
        WindowRectangle{130, 130, 1100, 820});
  // A 150% requested frame must still fit the physical work area.
  Check(FitWindowToWorkArea({130, 130, 1650, 1230}, {0, 0, 1920, 1040}) ==
        WindowRectangle{130, 0, 1650, 1040});
  Check(FitWindowToWorkArea({130, 130, 1100, 820}, {-1280, -200, 1280, 672}) ==
        WindowRectangle{-1100, -200, 1100, 672});
  Check(FitWindowToWorkArea({-3000, -3000, 2000, 2000}, {-1280, -200, 1280, 672}) ==
        WindowRectangle{-1280, -200, 1280, 672});
  Check(FitWindowToWorkArea({5, 5, 0, -1}, {10, 20, 1, 1}) ==
        WindowRectangle{10, 20, 1, 1});
  Check(FitWindowToWorkArea({130, 130, 1100, 820}, {0, 0, 0, 0}) ==
        WindowRectangle{130, 130, 1100, 820});
}
