#pragma once

#include <array>
#include <optional>

#include "ecu/signal.hpp"

namespace ecu {

enum class InsertResult {
    inserted,
    duplicate_id,
    unknown_id,
};

class InputDiagnostics {
public:
    [[nodiscard]] bool hasUnknownId() const noexcept;
    [[nodiscard]] bool hasDuplicate(SignalId id) const noexcept;

private:
    friend class SignalBatch;

    std::array<bool, signalCount> duplicates_{};
    bool unknownId_{false};
};

// Se prepara un lote por ciclo. Dos o más ocurrencias invalidan la ranura de ese ID.
class SignalBatch {
public:
    InsertResult add(SignalId id, double value) noexcept;

private:
    friend class Gateway;

    std::array<std::optional<double>, signalCount> readings_{};
    InputDiagnostics diagnostics_{};
};

class SignalSnapshot {
public:
    [[nodiscard]] Cycle cycle() const noexcept;
    [[nodiscard]] InputDiagnostics diagnostics() const noexcept;

    // Devuelve nullopt para un ID desconocido. Una señal sin muestra conserva su observación.
    [[nodiscard]] std::optional<SignalObservation> observe(SignalId id) const noexcept;

private:
    friend class Gateway;

    std::array<std::optional<SignalSample>, signalCount> samples_{};
    InputDiagnostics diagnostics_{};
    Cycle cycle_{0};
};

class Gateway {
public:
    [[nodiscard]] SignalSnapshot snapshot() const noexcept;

    // El ciclo 0 describe el arranque. Un ciclo repetido o anterior se rechaza sin cambios.
    [[nodiscard]] std::optional<SignalSnapshot> processCycle(
        Cycle cycle, const SignalBatch& batch) noexcept;

private:
    SignalSnapshot current_{};
};

}  // namespace ecu
