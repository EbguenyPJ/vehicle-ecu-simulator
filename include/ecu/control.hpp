#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "ecu/gateway.hpp"

namespace ecu {

enum class ControlState {
    init,
    operational,
    degraded,
    safe_state,
};

enum class ControlAction {
    inhibit_output,
    limited_operation,
    normal_operation,
};

enum class Severity {
    degraded,
    critical,
};

enum class DiagnosticCode {
    signal_missing,
    signal_expired,
    signal_retained,
    signal_out_of_range,
    signal_non_finite,
    coolant_over_temperature,
    low_oil_pressure,
    battery_voltage_outside_operating_range,
    powertrain_incoherence,
    recovery_pending,
};

struct Diagnostic {
    DiagnosticCode code;
    Severity severity;
    std::optional<SignalId> signal;
};

class Diagnostics {
public:
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const Diagnostic* begin() const noexcept { return findings_.data(); }
    [[nodiscard]] const Diagnostic* end() const noexcept { return begin() + size_; }
    [[nodiscard]] std::optional<Diagnostic> primary() const noexcept;

private:
    friend class ControlECU;

    void add(DiagnosticCode code, Severity severity,
             std::optional<SignalId> signal = std::nullopt) noexcept;

    // Como máximo un hallazgo por canal más uno de plausibilidad.
    std::array<Diagnostic, signalCount + 1> findings_{};
    std::size_t size_{0};
};

struct LatchedFault {
    Cycle cycle;
    Diagnostic cause;
};

struct ControlResult {
    Cycle cycle;
    ControlState state;
    ControlAction action;
    Diagnostics diagnostics;
    std::optional<LatchedFault> latchedFault;
};

class ControlECU {
public:
    // El ciclo debe ser mayor que el último aceptado. La instantánea se usa solo en esta llamada.
    [[nodiscard]] std::optional<ControlResult> step(const SignalSnapshot& snapshot) noexcept;

    // Rearma SAFE_STATE para un nuevo arranque, conservando el orden de ciclos.
    [[nodiscard]] bool reset() noexcept;

private:
    [[nodiscard]] static Diagnostics evaluate(
        const SignalSnapshot& snapshot, bool waitingForStartup) noexcept;

    ControlState state_{ControlState::init};
    Cycle lastCycle_{0};
    Cycle startupCycle_{0};
    bool previousCycleHealthy_{false};
    std::optional<LatchedFault> latchedFault_;
};

}  // namespace ecu
