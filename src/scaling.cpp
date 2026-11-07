#include "ecu/scaling.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ecu {

std::optional<LinearScale> scalingFor(SignalId id) noexcept
{
    switch (id) {
    case SignalId::coolant_temperature:
        return coolantTemperatureScale;
    case SignalId::battery_voltage:
        return batteryVoltageScale;
    default:
        return std::nullopt;
    }
}

std::optional<RepresentableRange> representableRange(const LinearScale& scale) noexcept
{
    if (!std::isfinite(scale.factor) || scale.factor == 0.0 || !std::isfinite(scale.offset) ||
        scale.rawBits == 0 || scale.rawBits > std::numeric_limits<std::uint16_t>::digits) {
        return std::nullopt;
    }

    const auto rawMaximum = static_cast<std::uint16_t>(
        (std::uint32_t{1} << scale.rawBits) - std::uint32_t{1});
    const double last = std::fma(static_cast<double>(rawMaximum), scale.factor, scale.offset);
    if (!std::isfinite(last) || last == scale.offset) {
        return std::nullopt;
    }
    return RepresentableRange{
        std::min(scale.offset, last), std::max(scale.offset, last), rawMaximum};
}

std::optional<EncodedValue> encodePhysical(double physical, const LinearScale& scale) noexcept
{
    const auto range = representableRange(scale);
    if (!range || !std::isfinite(physical)) {
        return std::nullopt;
    }

    if (physical <= range->minimum) {
        return EncodedValue{scale.factor > 0.0 ? std::uint16_t{0} : range->rawMaximum,
            physical < range->minimum ? Saturation::below_range : Saturation::none};
    }
    if (physical >= range->maximum) {
        return EncodedValue{scale.factor > 0.0 ? range->rawMaximum : std::uint16_t{0},
            physical > range->maximum ? Saturation::above_range : Saturation::none};
    }

    const double difference = physical - scale.offset;
    // La división separada evita desbordar una resta de operandos de signos opuestos.
    const double coordinate = std::isfinite(difference) ? difference / scale.factor
        : physical / scale.factor - scale.offset / scale.factor;
    if (!std::isfinite(coordinate)) {
        return std::nullopt;
    }
    const double bounded = std::clamp(coordinate, 0.0, static_cast<double>(range->rawMaximum));
    const double rounded = std::round(bounded);
    return EncodedValue{static_cast<std::uint16_t>(rounded), Saturation::none};
}

std::optional<double> decodeRaw(std::uint32_t raw, const LinearScale& scale) noexcept
{
    const auto range = representableRange(scale);
    if (!range || raw > range->rawMaximum) {
        return std::nullopt;
    }
    return std::fma(static_cast<double>(raw), scale.factor, scale.offset);
}

}  // namespace ecu
