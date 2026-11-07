#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace ecu {

using Cycle = std::uint64_t;

enum class SignalId : std::uint8_t {
    speed,
    engine_rpm,
    coolant_temperature,
    throttle_position,
    battery_voltage,
    oil_pressure,
};

struct SignalDescriptor {
    SignalId id{SignalId::speed};
    std::string_view unit{};
    double minimum{0.0};
    double maximum{0.0};
    Cycle toleratedMissingCycles{0};
};

inline constexpr std::array signalCatalog{
    SignalDescriptor{SignalId::speed,                "km/h",   0.0,  250.0, 3},
    SignalDescriptor{SignalId::engine_rpm,            "rpm",    0.0, 8000.0, 3},
    SignalDescriptor{SignalId::coolant_temperature,   "degC", -40.0,  150.0, 3},
    SignalDescriptor{SignalId::throttle_position,    "%",      0.0,  100.0, 3},
    SignalDescriptor{SignalId::battery_voltage,      "V",      9.0,   16.0, 3},
    SignalDescriptor{SignalId::oil_pressure,         "bar",    0.0,   10.0, 3},
};

inline constexpr std::size_t signalCount = signalCatalog.size();

enum class SampleQuality {
    in_range,
    out_of_range,
    non_finite,
};

enum class Freshness {
    never_received,
    fresh,
    retained,
    expired,
};

struct SignalSample {
    double value{std::numeric_limits<double>::quiet_NaN()};
    SampleQuality quality{SampleQuality::non_finite};
    Cycle receivedAt{0};
};

struct SignalObservation {
    std::optional<SignalSample> sample;
    std::optional<Cycle> age;
    Freshness freshness{Freshness::never_received};
};

namespace detail {

constexpr bool catalogIsValid() noexcept
{
    constexpr double maxFinite = std::numeric_limits<double>::max();

    for (std::size_t index = 0; index < signalCount; ++index) {
        const auto& signal = signalCatalog[index];
        if (static_cast<std::size_t>(signal.id) != index || signal.unit.empty() ||
            !(signal.minimum >= -maxFinite && signal.maximum <= maxFinite &&
              signal.minimum < signal.maximum)) {
            return false;
        }
    }
    return true;
}

static_assert(catalogIsValid(), "Signal IDs must match their slots and ranges must be finite and ordered");

constexpr std::optional<std::size_t> signalIndex(SignalId id) noexcept
{
    const auto index = static_cast<std::size_t>(id);
    if (index >= signalCount) {
        return std::nullopt;
    }
    return index;
}

}  // namespace detail
}  // namespace ecu
