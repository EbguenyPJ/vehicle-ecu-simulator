#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "ecu/gateway.hpp"

namespace ecu {

enum class ControlState {
    init,
    self_test,
    operational,
    degraded,
    safe_state,
    shutdown,
};

enum class ControlEvent {
    none,
    shutdown,
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
    DiagnosticCode code{DiagnosticCode::signal_missing};
    Severity severity{Severity::degraded};
    std::optional<SignalId> signal{};
};

class Diagnostics {
public:
    static constexpr std::size_t capacity = signalCount + 1;
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const Diagnostic* begin() const noexcept { return findings_.data(); }
    [[nodiscard]] const Diagnostic* end() const noexcept { return begin() + size_; }
    [[nodiscard]] std::optional<Diagnostic> primary() const noexcept;

private:
    friend class ControlECU;

    void add(DiagnosticCode code, Severity severity,
             std::optional<SignalId> signal = std::nullopt) noexcept;

    // Como máximo un hallazgo por canal más uno de plausibilidad.
    std::array<Diagnostic, capacity> findings_{};
    std::size_t size_{0};
};

struct LatchedFault {
    Cycle cycle{0};
    Diagnostic cause{};
};

struct ControlResult {
    Cycle cycle{0};
    ControlState state{ControlState::init};
    ControlAction action{ControlAction::inhibit_output};
    Diagnostics diagnostics{};
    std::optional<LatchedFault> latchedFault{};
};

class ControlECU {
public:
    // Arranca en INIT; el primer ciclo no crítico publica SELF_TEST antes de operar.
    [[nodiscard]] ControlState state() const noexcept { return state_; }

    // Ciclos crecientes; SHUTDOWN es terminal. El evento solo actúa en un ciclo aceptado.
    // El apagado conserva diagnósticos actuales y la primera causa crítica enclavada.
    [[nodiscard]] std::optional<ControlResult> step(
        const SignalSnapshot& snapshot, ControlEvent event = ControlEvent::none) noexcept;

    // Solo rearma SAFE_STATE: vuelve a INIT y conserva el orden de ciclos.
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
