#include "ecu/control.hpp"

#include <cassert>

namespace ecu {
namespace {

// Parámetros del escenario de motor simulado y marcha fija acoplada.
constexpr Cycle startupGraceCycles = 2;
constexpr double maxCoolantTemperatureC = 110.0;
constexpr double minOilPressureBar = 0.8;
constexpr double engineRunningRpm = 500.0;
constexpr double minBatteryVoltageV = 11.5;
constexpr double maxBatteryVoltageV = 15.0;
constexpr double minCoupledSpeedKmh = 20.0;
constexpr double minCoupledThrottlePercent = 20.0;
constexpr double minRpmPerKmh = 30.0;
constexpr double maxRpmPerKmh = 60.0;

ControlAction actionFor(ControlState state) noexcept
{
    switch (state) {
    case ControlState::operational:
        return ControlAction::normal_operation;
    case ControlState::degraded:
        return ControlAction::limited_operation;
    case ControlState::init:
    case ControlState::safe_state:
        return ControlAction::inhibit_output;
    }
    return ControlAction::inhibit_output;
}

}  // namespace

void Diagnostics::add(DiagnosticCode code, Severity severity, std::optional<SignalId> signal) noexcept
{
    assert(size_ < findings_.size());
    findings_[size_] = {code, severity, signal};
    ++size_;
}

std::optional<Diagnostic> Diagnostics::primary() const noexcept
{
    for (const auto& finding : *this) {
        if (finding.severity == Severity::critical) {
            return finding;
        }
    }
    if (size_ != 0) {
        return findings_.front();
    }
    return std::nullopt;
}

Diagnostics ControlECU::evaluate(const SignalSnapshot& snapshot, bool waitingForStartup) noexcept
{
    Diagnostics diagnostics;
    std::optional<double> speed;
    std::optional<double> rpm;
    std::optional<double> throttle;
    std::optional<double> oilPressure;

    for (const auto& descriptor : signalCatalog) {
        const auto id = descriptor.id;
        const auto observation = snapshot.observe(id);
        const bool critical = id == SignalId::coolant_temperature || id == SignalId::oil_pressure;
        const auto severity = critical ? Severity::critical : Severity::degraded;

        if (!observation || !observation->sample) {
            diagnostics.add(DiagnosticCode::signal_missing,
                            waitingForStartup ? Severity::degraded : severity, id);
            continue;
        }
        if (observation->freshness == Freshness::expired) {
            diagnostics.add(DiagnosticCode::signal_expired, severity, id);
            continue;
        }
        const auto& sample = *observation->sample;
        if (sample.quality != SampleQuality::in_range) {
            const auto code = sample.quality == SampleQuality::non_finite
                ? DiagnosticCode::signal_non_finite : DiagnosticCode::signal_out_of_range;
            diagnostics.add(code, severity, id);
            continue;
        }
        if (observation->freshness == Freshness::retained) {
            diagnostics.add(DiagnosticCode::signal_retained, Severity::degraded, id);
            continue;
        }

        switch (id) {
        case SignalId::speed:
            speed = sample.value;
            break;
        case SignalId::engine_rpm:
            rpm = sample.value;
            break;
        case SignalId::throttle_position:
            throttle = sample.value;
            break;
        case SignalId::oil_pressure:
            oilPressure = sample.value;
            break;
        case SignalId::coolant_temperature:
            if (sample.value > maxCoolantTemperatureC) {
                diagnostics.add(DiagnosticCode::coolant_over_temperature, Severity::critical, id);
            }
            break;
        case SignalId::battery_voltage:
            if (sample.value < minBatteryVoltageV || sample.value > maxBatteryVoltageV) {
                diagnostics.add(DiagnosticCode::battery_voltage_outside_operating_range,
                                Severity::degraded, id);
            }
            break;
        }
    }

    // Solo una muestra fresca y válida de RPM permite confirmar que el motor está detenido.
    const bool engineStopped = rpm && *rpm < engineRunningRpm;
    if (oilPressure && *oilPressure < minOilPressureBar && !engineStopped) {
        diagnostics.add(DiagnosticCode::low_oil_pressure, Severity::critical, SignalId::oil_pressure);
    }

    // Este diagnóstico depende exclusivamente de estas tres entradas frescas y válidas.
    if (speed && rpm && throttle && *speed >= minCoupledSpeedKmh &&
        *throttle >= minCoupledThrottlePercent &&
        (*rpm < *speed * minRpmPerKmh || *rpm > *speed * maxRpmPerKmh)) {
        diagnostics.add(DiagnosticCode::powertrain_incoherence, Severity::critical);
    }

    return diagnostics;
}

std::optional<ControlResult> ControlECU::step(const SignalSnapshot& snapshot) noexcept
{
    const Cycle cycle = snapshot.cycle();
    if (cycle <= lastCycle_) {
        return std::nullopt;
    }

    if (state_ == ControlState::init && startupCycle_ == 0) {
        startupCycle_ = cycle;
    }
    const bool waitingForStartup =
        state_ == ControlState::init && cycle - startupCycle_ < startupGraceCycles;
    auto diagnostics = evaluate(snapshot, waitingForStartup);
    const auto primary = diagnostics.primary();
    const bool healthy = !primary.has_value();
    const bool confirmedRecovery = previousCycleHealthy_ && cycle - lastCycle_ == 1;

    if (state_ != ControlState::safe_state) {
        if (primary && primary->severity == Severity::critical) {
            state_ = ControlState::safe_state;
            latchedFault_ = LatchedFault{cycle, *primary};
        } else if (healthy) {
            if (state_ == ControlState::degraded && !confirmedRecovery) {
                diagnostics.add(DiagnosticCode::recovery_pending, Severity::degraded);
            } else {
                state_ = ControlState::operational;
            }
        } else if (!waitingForStartup) {
            state_ = ControlState::degraded;
        }
    }

    lastCycle_ = cycle;
    previousCycleHealthy_ = healthy;
    return ControlResult{cycle, state_, actionFor(state_), diagnostics, latchedFault_};
}

bool ControlECU::reset() noexcept
{
    if (state_ != ControlState::safe_state) {
        return false;
    }

    state_ = ControlState::init;
    startupCycle_ = 0;
    previousCycleHealthy_ = false;
    latchedFault_.reset();
    return true;
}

}  // namespace ecu
