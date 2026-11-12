#pragma once

#include <type_traits>

#include "ecu/control.hpp"
#include "ecu/simulator.hpp"

namespace ecu {

// El observador recibe datos del mismo ciclo válidos durante la llamada.
// Un retorno bool igual a false detiene el recorrido y se propaga al llamador.
// El último ciclo declarado solicita el apagado de Control, sin añadir otro ciclo.
template <typename Observer>
[[nodiscard]] bool runScenario(const Scenario& scenario, Observer&& observe)
{
    const Cycle lastCycle = scenario.cycles();
    if (lastCycle == 0) {
        return false;
    }

    VehicleSimulator simulator;
    Gateway gateway;
    ControlECU control;
    Cycle cycle = 0;
    while (cycle < lastCycle) {
        ++cycle;
        const auto throttle = scenario.throttleAt(cycle);
        if (!throttle) {
            return false;
        }
        const auto nominal = simulator.step(*throttle);
        if (!nominal) {
            return false;
        }
        const auto batch = scenario.batchFor(cycle, *nominal);
        if (!batch) {
            return false;
        }

        const auto snapshot = gateway.processCycle(cycle, *batch);
        if (!snapshot) {
            return false;
        }
        const auto event = cycle == lastCycle ? ControlEvent::shutdown : ControlEvent::none;
        const auto result = control.step(*snapshot, event);
        if (!result) {
            return false;
        }
        using ObservationResult = std::invoke_result_t<Observer&, const SignalSnapshot&, const ControlResult&>;
        if constexpr (std::is_same_v<ObservationResult, bool>) {
            if (!observe(*snapshot, *result)) {
                return false;
            }
        } else {
            observe(*snapshot, *result);
        }
    }
    return true;
}

}  // namespace ecu
