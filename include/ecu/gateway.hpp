#pragma once

#include <array>
#include <optional>

#include "ecu/signal.hpp"

namespace ecu {

struct GatewayConfig {
    // Límite inclusivo de edad para todas las señales. Cero exige recepción en el ciclo actual.
    Cycle maxAgeCycles{3};
};

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
    [[nodiscard]] GatewayConfig config() const noexcept;

    // Devuelve nullopt para un ID desconocido. La observación incluye VALID/INVALID/MISSING.
    [[nodiscard]] std::optional<SignalObservation> observe(SignalId id) const noexcept;

private:
    friend class Gateway;

    std::array<std::optional<SignalSample>, signalCount> samples_{};
    InputDiagnostics diagnostics_{};
    Cycle cycle_{0};
    GatewayConfig config_{};
};

class Gateway {
public:
    Gateway() noexcept = default;
    // La configuración se copia; las instantáneas conservan su propia política temporal.
    explicit Gateway(GatewayConfig config) noexcept;

    [[nodiscard]] SignalSnapshot snapshot() const noexcept;

    // El ciclo 0 describe el arranque. Un ciclo repetido o anterior se rechaza sin cambios.
    [[nodiscard]] std::optional<SignalSnapshot> processCycle(
        Cycle cycle, const SignalBatch& batch) noexcept;

private:
    SignalSnapshot current_{};
};

}  // namespace ecu
