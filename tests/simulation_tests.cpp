#include "ecu/simulation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace ecu;

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

SignalObservation observationOf(const SignalSnapshot& snapshot, SignalId signal)
{
    const auto observation = snapshot.observe(signal);
    require(observation.has_value(), "a catalog signal must have an observation");
    return *observation;
}

double freshValue(const SignalSnapshot& snapshot, SignalId signal)
{
    const auto observation = observationOf(snapshot, signal);
    require(observation.sample && observation.freshness == Freshness::fresh &&
                observation.age == Cycle{0} && observation.sample->receivedAt == snapshot.cycle(),
            "a current sample must be fresh");
    require(observation.sample->quality == SampleQuality::in_range, "a nominal sample must be valid");
    return observation.sample->value;
}

void expectSameReadings(const VehicleReadings& lhs, const VehicleReadings& rhs)
{
    require(lhs.speedKmh == rhs.speedKmh && lhs.engineRpm == rhs.engineRpm &&
                lhs.coolantTemperatureC == rhs.coolantTemperatureC && lhs.throttlePercent == rhs.throttlePercent &&
                lhs.batteryVoltage == rhs.batteryVoltage && lhs.oilPressureBar == rhs.oilPressureBar,
            "rejecting a pedal input must not mutate the simulated plant");
}

void nominal()
{
    VehicleSimulator tested;
    VehicleSimulator reference;
    require(tested.step(25.0) && reference.step(25.0), "a nominal pedal input must advance the plant");
    const double infinity = std::numeric_limits<double>::infinity();
    for (const double invalid : {-1.0, 101.0, infinity, -infinity, std::numeric_limits<double>::quiet_NaN()}) {
        require(!tested.step(invalid), "invalid pedal inputs must be rejected");
    }
    const auto next = tested.step(100.0);
    const auto expected = reference.step(100.0);
    require(next && expected, "the upper pedal boundary must be valid");
    expectSameReadings(*next, *expected);

    const auto scenario = makeScenario(ScenarioKind::nominal);
    require(scenario && scenario->cycles() == 42, "the nominal drive must contain 42 ticks");
    constexpr std::array checkpoints{Cycle{4}, Cycle{22}, Cycle{42}};
    struct PlantSample {
        double speed;
        double rpm;
        double temperature;
        double pressure;
    };
    std::array<PlantSample, checkpoints.size()> samples{};
    Cycle observed = 0;
    const bool finished = runScenario(*scenario, [&](const SignalSnapshot& snapshot, const ControlResult& result) {
        ++observed;
        require(snapshot.cycle() == observed && result.cycle == observed, "Gateway and Control must share each tick");
        require(result.state == ControlState::operational && result.action == ControlAction::normal_operation &&
                    result.diagnostics.size() == 0 && !result.latchedFault,
                "a nominal drive must remain operational");
        for (const auto& descriptor : signalCatalog) {
            require(std::isfinite(freshValue(snapshot, descriptor.id)), "every nominal signal must be finite");
        }
        require(freshValue(snapshot, SignalId::throttle_position) == scenario->throttleAt(observed),
                "the reported pedal position must follow the driving schedule");
        const auto checkpoint = std::find(checkpoints.begin(), checkpoints.end(), observed);
        if (checkpoint != checkpoints.end()) {
            const auto index = static_cast<std::size_t>(checkpoint - checkpoints.begin());
            samples[index] = {freshValue(snapshot, SignalId::speed), freshValue(snapshot, SignalId::engine_rpm),
                              freshValue(snapshot, SignalId::coolant_temperature),
                              freshValue(snapshot, SignalId::oil_pressure)};
        }
    });
    require(finished && observed == scenario->cycles(), "all ticks must execute without a terminal");
    require(samples[1].speed > samples[0].speed && samples[1].rpm > samples[0].rpm &&
                samples[1].temperature > samples[0].temperature && samples[1].pressure > samples[0].pressure,
            "acceleration must raise speed, RPM, temperature, and oil pressure");
    require(samples[2].speed < samples[1].speed && samples[2].rpm < samples[1].rpm &&
                samples[2].pressure < samples[1].pressure,
            "releasing the pedal must reduce speed, RPM, and oil pressure");
}

void faultSchedule()
{
    Scenario scenario;
    require(scenario.cycles() == 0 && !scenario.throttleAt(1), "an empty scenario has no driving ticks");
    require(!runScenario(scenario, [](const SignalSnapshot&, const ControlResult&) {
        require(false, "an empty scenario cannot publish a tick");
    }), "an empty scenario must be rejected");

    require(scenario.addDriveSegment(3, 25.0) && scenario.addDriveSegment(6, 50.0),
            "increasing segment ends must be accepted");
    require(!scenario.addDriveSegment(0, 25.0) && !scenario.addDriveSegment(6, 25.0) &&
                !scenario.addDriveSegment(7, -1.0) && !scenario.addDriveSegment(7, 101.0) &&
                !scenario.addDriveSegment(7, std::numeric_limits<double>::quiet_NaN()),
            "non-increasing segment ends and invalid pedal settings must be rejected");
    require(scenario.cycles() == 6 && scenario.throttleAt(3) == 25.0 && scenario.throttleAt(4) == 50.0 &&
                !scenario.throttleAt(0) && !scenario.throttleAt(7),
            "the driving schedule must keep the declared cycle boundaries");

    const double infinity = std::numeric_limits<double>::infinity();
    require(scenario.addFault({SignalId::coolant_temperature, 2, 4, infinity}) &&
                scenario.addFault({SignalId::battery_voltage, 3, 4, std::nullopt}) &&
                scenario.addFault({SignalId::battery_voltage, 5, 5, 12.0}),
            "concurrent faults on different signals and adjacent intervals must be accepted");
    require(!scenario.addFault({SignalId::battery_voltage, 2, 3, 9.0}) &&
                !scenario.addFault({SignalId::battery_voltage, 4, 5, 9.0}) &&
                !scenario.addFault({static_cast<SignalId>(255), 1, 1, 0.0}) &&
                !scenario.addFault({SignalId::speed, 0, 1, 0.0}) &&
                !scenario.addFault({SignalId::speed, 4, 3, 0.0}) &&
                !scenario.addFault({SignalId::speed, 6, 7, 0.0}),
            "intersections, unknown IDs, and invalid fault boundaries must be rejected");

    VehicleSimulator simulator;
    Gateway gateway;
    std::optional<double> batteryAtTwo;
    for (Cycle cycle = 1; cycle <= scenario.cycles(); ++cycle) {
        const auto throttle = scenario.throttleAt(cycle);
        require(throttle.has_value(), "a declared driving tick must have a pedal setting");
        const auto readings = simulator.step(*throttle);
        require(readings.has_value(), "the declared pedal setting must advance the plant");
        require(!scenario.batchFor(0, *readings) && !scenario.batchFor(7, *readings),
                "the schedule must reject batches outside the driving range");
        const auto batch = scenario.batchFor(cycle, *readings);
        require(batch.has_value(), "every declared driving tick must produce a batch");
        const auto snapshot = gateway.processCycle(cycle, *batch);
        require(snapshot.has_value(), "the Gateway must publish the scheduled batch");

        require(freshValue(*snapshot, SignalId::speed) == readings->speedKmh &&
                    freshValue(*snapshot, SignalId::engine_rpm) == readings->engineRpm &&
                    freshValue(*snapshot, SignalId::throttle_position) == readings->throttlePercent &&
                    freshValue(*snapshot, SignalId::oil_pressure) == readings->oilPressureBar,
                "faults must leave the unrelated readings unchanged");

        if (cycle >= 2 && cycle <= 4) {
            const auto coolant = observationOf(*snapshot, SignalId::coolant_temperature);
            require(coolant.sample && coolant.freshness == Freshness::fresh && coolant.sample->value == infinity &&
                        coolant.sample->quality == SampleQuality::non_finite,
                    "a replacement must reach Gateway unchanged throughout the inclusive interval");
        } else {
            require(freshValue(*snapshot, SignalId::coolant_temperature) == readings->coolantTemperatureC,
                    "temperature must use the plant before and after the fault");
        }
        if (cycle == 3 || cycle == 4) {
            const auto battery = observationOf(*snapshot, SignalId::battery_voltage);
            require(battery.sample && battery.sample->receivedAt == 2 && battery.sample->value == batteryAtTwo &&
                        battery.freshness == Freshness::retained && battery.age == cycle - 2,
                    "omitting a reading must preserve the last battery sample and age it");
        } else {
            const double battery = freshValue(*snapshot, SignalId::battery_voltage);
            require(battery == (cycle == 5 ? 12.0 : readings->batteryVoltage),
                    "a battery replacement must be limited to its declared cycle");
            if (cycle == 2) {
                batteryAtTwo = battery;
            }
        }
    }

    Scenario fullDrive;
    for (Cycle cycle = 1; cycle <= Scenario::maxDriveSegments; ++cycle) {
        require(fullDrive.addDriveSegment(cycle, 0.0), "the declared drive capacity must be usable");
    }
    require(!fullDrive.addDriveSegment(Scenario::maxDriveSegments + 1, 0.0) &&
                fullDrive.cycles() == Scenario::maxDriveSegments,
            "a full drive schedule must reject new segments without changing its duration");

    Scenario fullFaults;
    require(fullFaults.addDriveSegment(Scenario::maxFaultEvents + 1, 0.0),
            "a single segment must cover the fault capacity test");
    for (Cycle cycle = 1; cycle <= Scenario::maxFaultEvents; ++cycle) {
        require(fullFaults.addFault({SignalId::speed, cycle, cycle, std::nullopt}),
                "the declared fault capacity must be usable");
    }
    constexpr Cycle extraCycle = Scenario::maxFaultEvents + 1;
    require(!fullFaults.addFault({SignalId::speed, extraCycle, extraCycle, std::nullopt}),
            "a full fault schedule must reject one more event");
}

bool sameDiagnostic(const Diagnostic& lhs, const Diagnostic& rhs)
{
    return lhs.code == rhs.code && lhs.severity == rhs.severity && lhs.signal == rhs.signal;
}

void expectDiagnostic(const ControlResult& result, DiagnosticCode code,
                      Severity severity, std::optional<SignalId> signal = std::nullopt)
{
    const Diagnostic expected{code, severity, signal};
    require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [&expected](const Diagnostic& current) { return sameDiagnostic(current, expected); }),
            "the expected cycle diagnostic must be present");
}

struct Tick {
    SignalSnapshot snapshot;
    ControlResult control;
};

void expectSameTick(const Tick& first, const SignalSnapshot& snapshot, const ControlResult& result)
{
    require(first.snapshot.cycle() == snapshot.cycle() && first.control.cycle == result.cycle &&
                first.control.state == result.state && first.control.action == result.action &&
                std::equal(first.control.diagnostics.begin(), first.control.diagnostics.end(),
                           result.diagnostics.begin(), result.diagnostics.end(), sameDiagnostic),
            "a second run must reproduce the Control result");
    require(first.control.latchedFault.has_value() == result.latchedFault.has_value(),
            "a second run must reproduce the presence of the latch");
    if (result.latchedFault) {
        require(first.control.latchedFault->cycle == result.latchedFault->cycle &&
                    sameDiagnostic(first.control.latchedFault->cause, result.latchedFault->cause),
                "a second run must reproduce the cycle and cause of the latch");
    }

    for (const auto& descriptor : signalCatalog) {
        const auto original = observationOf(first.snapshot, descriptor.id);
        const auto repeated = observationOf(snapshot, descriptor.id);
        require(original.freshness == repeated.freshness && original.age == repeated.age &&
                    original.sample.has_value() == repeated.sample.has_value(),
                "a second run must reproduce the presence and freshness of each sample");
        if (original.sample) {
            require(original.sample->value == repeated.sample->value &&
                        original.sample->quality == repeated.sample->quality &&
                        original.sample->receivedAt == repeated.sample->receivedAt,
                    "a second run must reproduce every sample value, quality, and origin tick");
        }
    }
}

void integration()
{
    const auto scenario = makeScenario(ScenarioKind::faults);
    const auto baseline = makeScenario(ScenarioKind::nominal);
    require(scenario && baseline && scenario->cycles() == 42 && baseline->cycles() == scenario->cycles(),
            "the nominal and fault runs must share the same driving duration");

    constexpr std::size_t ticks = 42;
    std::array<std::optional<Tick>, ticks> trace{};
    VehicleSimulator nominalPlant;
    std::size_t seen = 0;
    const bool finished = runScenario(*scenario, [&](const SignalSnapshot& snapshot, const ControlResult& result) {
        require(seen < trace.size(), "the run must stop at its declared length");
        ++seen;
        const Cycle cycle = snapshot.cycle();
        require(cycle == seen && result.cycle == cycle, "the pipeline must keep a common, contiguous cycle");
        trace[seen - 1] = Tick{snapshot, result};

        const auto nominalThrottle = baseline->throttleAt(cycle);
        require(nominalThrottle.has_value(), "the same tick must exist in the nominal driving script");
        const auto nominal = nominalPlant.step(*nominalThrottle);
        require(nominal.has_value(), "the nominal plant must advance independently of faults");
        const bool blackout = cycle >= 14 && cycle <= 17;
        if (blackout) {
            const auto age = cycle - 13;
            for (const auto& descriptor : signalCatalog) {
                const auto observation = observationOf(snapshot, descriptor.id);
                require(observation.sample && observation.sample->receivedAt == 13 && observation.age == age &&
                            observation.freshness == (cycle == 17 ? Freshness::expired : Freshness::retained),
                        "blackout must retain all samples until the fourth omitted tick expires them");
            }
        } else {
            require(freshValue(snapshot, SignalId::speed) == nominal->speedKmh &&
                        freshValue(snapshot, SignalId::engine_rpm) == nominal->engineRpm &&
                        freshValue(snapshot, SignalId::oil_pressure) == nominal->oilPressureBar,
                    "fault injection must never contaminate the nominal plant trajectory");
            if (cycle != 20) {
                require(freshValue(snapshot, SignalId::coolant_temperature) == nominal->coolantTemperatureC,
                        "temperature must return to the unmodified plant trajectory");
            }
        }

        if (cycle >= 17) {
            require(result.state == ControlState::safe_state && result.action == ControlAction::inhibit_output &&
                        result.latchedFault && result.latchedFault->cycle == 17,
                    "the first critical expiry must inhibit operation and stay latched");
            const Diagnostic expiry{DiagnosticCode::signal_expired, Severity::critical, SignalId::coolant_temperature};
            require(sameDiagnostic(result.latchedFault->cause, expiry), "the original cause must survive the rest of the drive");
        } else if (cycle == 5 || cycle == 6 || (cycle >= 9 && cycle <= 11) || (cycle >= 14 && cycle <= 16)) {
            require(result.state == ControlState::degraded && result.action == ControlAction::limited_operation,
                    "temporary losses and invalid noncritical samples must degrade operation");
        } else {
            require(result.state == ControlState::operational && result.action == ControlAction::normal_operation,
                    "healthy intervals must allow operation after recovery");
        }

        if (cycle == 5) {
            expectDiagnostic(result, DiagnosticCode::signal_retained, Severity::degraded, SignalId::battery_voltage);
        }
        if (cycle == 6 || cycle == 11) {
            expectDiagnostic(result, DiagnosticCode::recovery_pending, Severity::degraded);
        }
        if (cycle == 9 || cycle == 10) {
            const auto pedal = observationOf(snapshot, SignalId::throttle_position);
            require(pedal.sample && pedal.sample->value == 120.0 && pedal.sample->quality == SampleQuality::out_of_range,
                    "the injected pedal value must be detected at the Gateway boundary");
            expectDiagnostic(result, DiagnosticCode::signal_out_of_range, Severity::degraded, SignalId::throttle_position);
        }
        if (cycle == 18) {
            require(result.diagnostics.size() == 0, "inputs must recover after the blackout while SAFE_STATE remains");
        }
        if (cycle == 20) {
            require(freshValue(snapshot, SignalId::coolant_temperature) == 130.0,
                    "130 degrees must be an accepted, fresh measurement for the Gateway");
            expectDiagnostic(result, DiagnosticCode::coolant_over_temperature, Severity::critical, SignalId::coolant_temperature);
        }
    });
    require(finished && seen == trace.size(), "a simulated safe-state must not stop the headless scenario");

    std::size_t repeated = 0;
    const bool reran = runScenario(*scenario, [&](const SignalSnapshot& snapshot, const ControlResult& result) {
        require(repeated < trace.size() && trace[repeated], "the replay must match the original run length");
        expectSameTick(*trace[repeated], snapshot, result);
        ++repeated;
    });
    require(reran && repeated == trace.size(), "running the same script twice must reproduce every tick");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

constexpr std::array<TestCase, 3> cases{{
    {"nominal", nominal},
    {"fault_schedule", faultSchedule},
    {"integration", integration},
}};

}  // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Expected one test case name.\n";
        return EXIT_FAILURE;
    }

    for (const auto& test : cases) {
        if (test.name != argv[1]) {
            continue;
        }
        try {
            test.run();
        } catch (const std::exception& error) {
            std::cerr << test.name << ": " << error.what() << '\n';
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    std::cerr << "Unknown test case: " << argv[1] << '\n';
    return EXIT_FAILURE;
}
