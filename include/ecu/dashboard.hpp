#pragma once

#include <cstddef>
#include <iosfwd>

#include "ecu/control.hpp"

namespace ecu {

enum class DashboardFormat {
    ascii,
    braille,
};

inline constexpr std::size_t dashboardMinBrailleColumns = 80;

[[nodiscard]] DashboardFormat selectDashboardFormat(bool utf8Terminal, std::size_t columns) noexcept;

// La salida se produce durante la llamada. Cero columnas indica un ancho desconocido.
[[nodiscard]] bool renderDashboard(
    std::ostream& out, const SignalSnapshot& snapshot, const ControlResult& control,
    DashboardFormat format, std::size_t columns = dashboardMinBrailleColumns);

}  // namespace ecu
