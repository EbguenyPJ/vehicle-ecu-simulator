#include "ecu/control.hpp"
#include "ecu/dashboard.hpp"
#include "ecu/gateway.hpp"
#include "ecu/signal.hpp"
#include "ecu/simulation.hpp"
#include "ecu/simulator.hpp"

#include <cstdlib>
#include <sstream>

int main()
{
    ecu::Scenario scenario;
    if (!scenario.addDriveSegment(1, 0.0)) {
        return EXIT_FAILURE;
    }

    std::ostringstream output;
    bool rendered = false;
    const bool completed = ecu::runScenario(scenario, [&](const ecu::SignalSnapshot& snapshot,
                                                         const ecu::ControlResult& control) {
        rendered = ecu::renderDashboard(output, snapshot, control, ecu::DashboardFormat::ascii);
    });
    return completed && rendered && !output.str().empty() ? EXIT_SUCCESS : EXIT_FAILURE;
}
