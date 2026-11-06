#include "ecu/simulation.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

std::string_view stateName(ecu::ControlState state) noexcept
{
    switch (state) {
    case ecu::ControlState::init:
        return "INIT";
    case ecu::ControlState::operational:
        return "OPERATIONAL";
    case ecu::ControlState::degraded:
        return "DEGRADED";
    case ecu::ControlState::safe_state:
        return "SAFE_STATE";
    }
    return "UNKNOWN";
}

void printCycle(const ecu::SignalSnapshot&, const ecu::ControlResult& result)
{
    std::cout << "cycle=" << result.cycle << " state=" << stateName(result.state);
    if (result.latchedFault) {
        std::cout << " safe_since=" << result.latchedFault->cycle;
    }
    std::cout << '\n';
}

}  // namespace

int main(int argc, char* argv[])
{
    const std::string_view name = argc > 1 ? argv[1] : "nominal";
    if (argc > 2 || (name != "nominal" && name != "faults")) {
        std::cerr << "Uso: vehicle-ecu-simulator [nominal|faults]\n";
        return EXIT_FAILURE;
    }

    const auto kind = name == "faults" ? ecu::ScenarioKind::faults : ecu::ScenarioKind::nominal;
    const auto scenario = ecu::makeScenario(kind);
    if (!scenario || !ecu::runScenario(*scenario, printCycle)) {
        std::cerr << "No se pudo completar el escenario.\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
