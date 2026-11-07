#include "ecu/control.hpp"
#include "ecu/dashboard.hpp"
#include "ecu/gateway.hpp"
#include "ecu/signal.hpp"
#include "ecu/simulation.hpp"
#include "ecu/simulator.hpp"

#include <cstdlib>
#include <cstddef>
#include <sstream>

int main()
{
    ecu::Gateway gateway{ecu::GatewayConfig{0}};
    ecu::SignalBatch batch;
    batch.add(ecu::SignalId::battery_voltage, 13.8);
    const auto received = gateway.processCycle(10, batch);
    const auto expired = gateway.processCycle(11, ecu::SignalBatch{});
    if (!received || !expired || received->config().maxAgeCycles != 0) {
        return EXIT_FAILURE;
    }
    const auto valid = received->observe(ecu::SignalId::battery_voltage);
    const auto missing = expired->observe(ecu::SignalId::battery_voltage);
    if (!valid || !missing || valid->status != ecu::SignalStatus::valid ||
        missing->status != ecu::SignalStatus::missing) {
        return EXIT_FAILURE;
    }

    ecu::Scenario scenario;
    if (!scenario.addDriveSegment(3, 0.0)) {
        return EXIT_FAILURE;
    }

    std::ostringstream output;
    bool rendered = true;
    std::size_t seen = 0;
    bool lifecycleMatches = true;
    const bool completed = ecu::runScenario(scenario, [&](const ecu::SignalSnapshot& snapshot,
                                                         const ecu::ControlResult& control) {
        ++seen;
        rendered = ecu::renderDashboard(output, snapshot, control, ecu::DashboardFormat::ascii) && rendered;
        const auto expected = seen == 1 ? ecu::ControlState::self_test
            : seen == 2 ? ecu::ControlState::operational : ecu::ControlState::shutdown;
        lifecycleMatches = lifecycleMatches && control.state == expected && snapshot.cycle() == seen &&
            control.cycle == seen;
    });
    ecu::ControlECU terminal;
    if (terminal.state() != ecu::ControlState::init) {
        return EXIT_FAILURE;
    }
    const auto stopped = terminal.step(*received, ecu::ControlEvent::shutdown);
    if (!stopped || stopped->state != ecu::ControlState::shutdown || terminal.reset() || terminal.step(*expired)) {
        return EXIT_FAILURE;
    }
    return completed && rendered && lifecycleMatches && seen == 3 && !output.str().empty()
        ? EXIT_SUCCESS : EXIT_FAILURE;
}
