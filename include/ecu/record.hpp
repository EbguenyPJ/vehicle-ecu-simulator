#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "ecu/control.hpp"

namespace ecu {

// Datos propios de un ciclo. Las posiciones de señales siguen signalCatalog.
struct CycleRecord {
    Cycle cycle{0};
    GatewayConfig gatewayConfig{};
    ControlState state{ControlState::init};
    ControlAction action{ControlAction::inhibit_output};
    std::array<SignalObservation, signalCount> signals{};
    bool unknownId{false};
    std::array<bool, signalCount> duplicates{};
    std::array<Diagnostic, Diagnostics::capacity> diagnostics{};
    std::size_t diagnosticCount{0};
    std::optional<LatchedFault> latchedFault{};
};

[[nodiscard]] bool isValidRecord(const CycleRecord& record) noexcept;
[[nodiscard]] std::optional<CycleRecord> makeRecord(
    const SignalSnapshot& snapshot, const ControlResult& result) noexcept;

// El destino implementa bool write(const CycleRecord&) y vive durante el recorrido.
template <typename Sink>
[[nodiscard]] auto recordObserver(Sink& sink)
{
    return [&sink](const SignalSnapshot& snapshot, const ControlResult& result) {
        const auto record = makeRecord(snapshot, result);
        return record && sink.write(*record);
    };
}

}  // namespace ecu
