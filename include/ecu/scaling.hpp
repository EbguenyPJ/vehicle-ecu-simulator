#pragma once

#include <cstdint>
#include <optional>

#include "ecu/signal.hpp"

namespace ecu {

struct LinearScale {
    double factor{1.0};
    double offset{0.0};
    std::uint8_t rawBits{16};
};

inline constexpr LinearScale coolantTemperatureScale{0.1, -40.0, 16};
inline constexpr LinearScale batteryVoltageScale{0.05, 8.0, 8};

// Los extremos se refieren al valor físico, incluso si el factor es negativo.
enum class Saturation {
    none,
    below_range,
    above_range,
};

struct EncodedValue {
    std::uint16_t raw{0};
    Saturation saturation{Saturation::none};
};

struct RepresentableRange {
    double minimum{0.0};
    double maximum{0.0};
    std::uint16_t rawMaximum{0};
};

// Solo temperatura y batería tienen un perfil de conversión definido.
[[nodiscard]] std::optional<LinearScale> scalingFor(SignalId id) noexcept;

// Rechaza factores cero/no finitos, offsets no finitos, bits fuera de 1..16
// y extremos físicos no finitos o indistinguibles en double.
[[nodiscard]] std::optional<RepresentableRange> representableRange(const LinearScale& scale) noexcept;

// Redondea el crudo calculado en double con std::round. No finitos: nullopt.
// Una entrada exterior al rango físico se satura antes de redondear.
[[nodiscard]] std::optional<EncodedValue> encodePhysical(double physical, const LinearScale& scale) noexcept;

// El argumento ancho permite rechazar exceso de bits antes de estrecharlo.
[[nodiscard]] std::optional<double> decodeRaw(std::uint32_t raw, const LinearScale& scale) noexcept;

}  // namespace ecu
