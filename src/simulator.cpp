#include "ecu/simulator.hpp"

#include <algorithm>

namespace ecu {
namespace {

constexpr bool validThrottle(double throttlePercent) noexcept
{
    return throttlePercent >= 0.0 && throttlePercent <= 100.0;
}

}  // namespace

std::optional<VehicleReadings> VehicleSimulator::step(double throttlePercent) noexcept
{
    if (!validThrottle(throttlePercent)) {
        return std::nullopt;
    }

    constexpr double maxAccelerationKmhPerSecond = 8.0;
    constexpr double maxDecelerationKmhPerSecond = 12.0;
    const double targetSpeedKmh = 1.2 * throttlePercent;
    speedKmh_ += std::clamp(targetSpeedKmh - speedKmh_,
                            -maxDecelerationKmhPerSecond * secondsPerCycle,
                            maxAccelerationKmhPerSecond * secondsPerCycle);

    const double engineRpm = std::max(800.0, 45.0 * speedKmh_);
    const double targetTemperatureC = 72.0 + 0.24 * throttlePercent;
    coolantTemperatureC_ += (targetTemperatureC - coolantTemperatureC_) * 0.06 * secondsPerCycle;
    const double batteryVoltage = 13.6 + 0.00004 * engineRpm - 0.0025 * throttlePercent;
    const double oilPressureBar = 1.2 + 0.0005 * engineRpm;

    return VehicleReadings{speedKmh_, engineRpm, coolantTemperatureC_,
                           throttlePercent, batteryVoltage, oilPressureBar};
}

bool Scenario::addDriveSegment(Cycle lastCycle, double throttlePercent) noexcept
{
    if (driveCount_ == drive_.size() || lastCycle <= cycles() || !validThrottle(throttlePercent)) {
        return false;
    }

    drive_[driveCount_] = {lastCycle, throttlePercent};
    ++driveCount_;
    return true;
}

bool Scenario::addFault(const FaultEvent& event) noexcept
{
    if (faultCount_ == faults_.size() || !detail::signalIndex(event.signal) ||
        event.firstCycle == 0 || event.firstCycle > event.lastCycle || event.lastCycle > cycles()) {
        return false;
    }

    for (std::size_t index = 0; index < faultCount_; ++index) {
        const auto& previous = faults_[index];
        if (previous.signal == event.signal &&
            event.firstCycle <= previous.lastCycle && previous.firstCycle <= event.lastCycle) {
            return false;
        }
    }

    faults_[faultCount_] = event;
    ++faultCount_;
    return true;
}

Cycle Scenario::cycles() const noexcept
{
    return driveCount_ == 0 ? 0 : drive_[driveCount_ - 1].lastCycle;
}

std::optional<double> Scenario::throttleAt(Cycle cycle) const noexcept
{
    if (cycle == 0) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index < driveCount_; ++index) {
        if (cycle <= drive_[index].lastCycle) {
            return drive_[index].throttlePercent;
        }
    }
    return std::nullopt;
}

std::optional<SignalBatch> Scenario::batchFor(Cycle cycle, const VehicleReadings& nominal) const noexcept
{
    if (cycle == 0 || cycle > cycles()) {
        return std::nullopt;
    }

    // Los IDs ya se validaron y los intervalos del mismo canal no se solapan.
    std::array<const FaultEvent*, signalCount> activeFaults{};
    for (std::size_t index = 0; index < faultCount_; ++index) {
        const auto& event = faults_[index];
        if (event.firstCycle <= cycle && cycle <= event.lastCycle) {
            activeFaults[static_cast<std::size_t>(event.signal)] = &event;
        }
    }

    SignalBatch batch;
    const auto emit = [&](SignalId id, double nominalValue) {
        const auto* fault = activeFaults[static_cast<std::size_t>(id)];
        if (fault == nullptr) {
            batch.add(id, nominalValue);
        } else if (fault->replacement) {
            batch.add(id, *fault->replacement);
        }
    };

    emit(SignalId::speed, nominal.speedKmh);
    emit(SignalId::engine_rpm, nominal.engineRpm);
    emit(SignalId::coolant_temperature, nominal.coolantTemperatureC);
    emit(SignalId::throttle_position, nominal.throttlePercent);
    emit(SignalId::battery_voltage, nominal.batteryVoltage);
    emit(SignalId::oil_pressure, nominal.oilPressureBar);
    return batch;
}

std::optional<Scenario> makeScenario(ScenarioKind kind) noexcept
{
    Scenario scenario;
    if (!scenario.addDriveSegment(4, 0.0) ||
        !scenario.addDriveSegment(22, 50.0) ||
        !scenario.addDriveSegment(32, 25.0) ||
        !scenario.addDriveSegment(42, 0.0)) {
        return std::nullopt;
    }

    switch (kind) {
    case ScenarioKind::nominal:
        return scenario;
    case ScenarioKind::faults:
        if (!scenario.addFault({SignalId::battery_voltage, 5, 5, std::nullopt}) ||
            !scenario.addFault({SignalId::throttle_position, 9, 10, 120.0}) ||
            !scenario.addFault({SignalId::coolant_temperature, 20, 20, 130.0})) {
            return std::nullopt;
        }
        for (const auto& signal : signalCatalog) {
            if (!scenario.addFault({signal.id, 14, 17, std::nullopt})) {
                return std::nullopt;
            }
        }
        return scenario;
    }
    return std::nullopt;
}

}  // namespace ecu
