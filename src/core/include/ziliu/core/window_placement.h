#pragma once

#include <algorithm>

namespace ziliu::core {

struct WindowRectangle {
  int left;
  int top;
  int width;
  int height;
  friend constexpr bool operator==(const WindowRectangle&, const WindowRectangle&) = default;
};

// Keep the complete frame reachable on the chosen monitor, including small
// desktops and monitors with negative virtual-desktop coordinates.
[[nodiscard]] constexpr WindowRectangle FitWindowToWorkArea(
    WindowRectangle preferred, WindowRectangle work_area) noexcept {
  if (work_area.width <= 0 || work_area.height <= 0) return preferred;
  const int width = std::clamp(preferred.width, 1, work_area.width);
  const int height = std::clamp(preferred.height, 1, work_area.height);
  return {std::clamp(preferred.left, work_area.left,
                     work_area.left + work_area.width - width),
          std::clamp(preferred.top, work_area.top,
                     work_area.top + work_area.height - height),
          width, height};
}

}  // namespace ziliu::core
