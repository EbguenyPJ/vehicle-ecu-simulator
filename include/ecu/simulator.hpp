#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "ecu/gateway.hpp"

namespace ecu {

struct VehicleReadings {
    double speedKmh;
    double engineRpm;
    double coolantTemperatureC;
    double throttlePercent;
    double batteryVoltage;
    double oilPressureBar;
};

class VehicleSimulator {
public:
    static constexpr double secondsPerCycle = 0.5;

    // Avanza un ciclo lógico. Un pedal inválido deja intacta la planta.
    [[nodiscard]] std::optional<VehicleReadings> step(double throttlePercent) noexcept;

private:
    double speedKmh_{0.0};
    double coolantTemperatureC_{25.0};
};

struct FaultEvent {
    SignalId signal;
    Cycle firstCycle;
    Cycle lastCycle;
    std::optional<double> replacement;  // Sin valor se omite la lectura.
};

class Scenario {
public:
    static constexpr std::size_t maxDriveSegments = 16;
    static constexpr std::size_t maxFaultEvents = 32;

    // El primer tramo empieza en 1 y los siguientes son contiguos.
    [[nodiscard]] bool addDriveSegment(Cycle lastCycle, double throttlePercent) noexcept;
    [[nodiscard]] bool addFault(FaultEvent event) noexcept;

    [[nodiscard]] Cycle cycles() const noexcept;
    [[nodiscard]] std::optional<double> throttleAt(Cycle cycle) const noexcept;
    [[nodiscard]] std::optional<SignalBatch> batchFor(
        Cycle cycle, const VehicleReadings& nominal) const noexcept;

private:
    struct DriveSegment {
        Cycle lastCycle;
        double throttlePercent;
    };

    std::array<DriveSegment, maxDriveSegments> drive_{};
    std::array<FaultEvent, maxFaultEvents> faults_{};
    std::size_t driveCount_{0};
    std::size_t faultCount_{0};
};

enum class ScenarioKind {
    nominal,
    faults,
};

[[nodiscard]] std::optional<Scenario> makeScenario(ScenarioKind kind) noexcept;

}  // namespace ecu
