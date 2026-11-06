#include "ecu/control.hpp"
#include "ecu/gateway.hpp"

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

struct Readings {
    std::optional<double> speed{60.0};
    std::optional<double> rpm{2660.0};
    std::optional<double> temperature{90.0};
    std::optional<double> throttle{30.0};
    std::optional<double> battery{13.8};
    std::optional<double> oil{2.5};
};

struct Scenario {
    Gateway gateway;
    ControlECU control;

    SignalSnapshot publish(Cycle cycle, const Readings& readings = {})
    {
        SignalBatch batch;
        const auto add = [&batch](SignalId id, std::optional<double> value) {
            if (value) {
                require(batch.add(id, *value) == InsertResult::inserted,
                        "a single reading must enter the batch");
            }
        };
        add(SignalId::speed, readings.speed);
        add(SignalId::engine_rpm, readings.rpm);
        add(SignalId::coolant_temperature, readings.temperature);
        add(SignalId::throttle_position, readings.throttle);
        add(SignalId::battery_voltage, readings.battery);
        add(SignalId::oil_pressure, readings.oil);

        const auto snapshot = gateway.processCycle(cycle, batch);
        require(snapshot.has_value(), "Gateway must publish each increasing cycle");
        return *snapshot;
    }

    ControlResult step(Cycle cycle, const Readings& readings = {})
    {
        const auto snapshot = publish(cycle, readings);
        const auto result = control.step(snapshot);
        require(result.has_value() && result->cycle == cycle,
                "Control must decide for the cycle published by Gateway");
        return *result;
    }
};

void expectState(const ControlResult& result, ControlState state, ControlAction action)
{
    if (result.state != state) {
        throw std::runtime_error("unexpected control state at cycle " + std::to_string(result.cycle));
    }
    if (result.action != action) {
        throw std::runtime_error("unexpected simulated control action at cycle " + std::to_string(result.cycle));
    }
}

void expectDiagnostic(const ControlResult& result, DiagnosticCode code, Severity severity,
                      std::optional<SignalId> signal = std::nullopt)
{
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == code && diagnostic.severity == severity &&
            diagnostic.signal == signal) {
            return;
        }
    }
    require(false, "expected diagnostic is missing");
}

bool hasCode(const ControlResult& result, DiagnosticCode code)
{
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

void expectPrimary(const ControlResult& result, DiagnosticCode code, Severity severity,
                   std::optional<SignalId> signal = std::nullopt)
{
    const auto primary = result.diagnostics.primary();
    require(primary.has_value(), "a diagnostic cause must be published");
    require(primary->code == code && primary->severity == severity && primary->signal == signal,
            "unexpected primary diagnostic");
}

void expectLatch(const ControlResult& result, Cycle cycle, DiagnosticCode code,
                 std::optional<SignalId> signal = std::nullopt)
{
    require(result.latchedFault.has_value(), "SAFE_STATE must record its entry fault");
    const auto& fault = *result.latchedFault;
    require(fault.cycle == cycle && fault.cause.code == code &&
                fault.cause.severity == Severity::critical && fault.cause.signal == signal,
            "unexpected latched entry fault");
}

void startup()
{
    Scenario nominal;
    require(!nominal.control.reset(), "INIT has no safe-state latch to reset");
    const auto healthy = nominal.step(1);
    expectState(healthy, ControlState::operational, ControlAction::normal_operation);
    require(healthy.diagnostics.size() == 0 && !healthy.latchedFault,
            "nominal startup must produce no faults");

    Readings incomplete;
    incomplete.oil.reset();
    Scenario criticalMissing;
    expectState(criticalMissing.step(10, incomplete), ControlState::init, ControlAction::inhibit_output);
    expectState(criticalMissing.step(11, incomplete), ControlState::init, ControlAction::inhibit_output);
    const auto timeout = criticalMissing.step(12, incomplete);
    expectState(timeout, ControlState::safe_state, ControlAction::inhibit_output);
    expectPrimary(timeout, DiagnosticCode::signal_missing, Severity::critical, SignalId::oil_pressure);
    expectLatch(timeout, 12, DiagnosticCode::signal_missing, SignalId::oil_pressure);

    incomplete.oil = 2.5;
    incomplete.battery.reset();
    Scenario noncriticalMissing;
    expectState(noncriticalMissing.step(10, incomplete), ControlState::init, ControlAction::inhibit_output);
    const auto limited = noncriticalMissing.step(12, incomplete);
    expectState(limited, ControlState::degraded, ControlAction::limited_operation);
    expectPrimary(limited, DiagnosticCode::signal_missing, Severity::degraded, SignalId::battery_voltage);

    incomplete.temperature = 130.0;
    Scenario immediateFault;
    const auto fault = immediateFault.step(1, incomplete);
    expectState(fault, ControlState::safe_state, ControlAction::inhibit_output);
    expectPrimary(fault, DiagnosticCode::coolant_over_temperature, Severity::critical,
                  SignalId::coolant_temperature);
}

void degradation()
{
    Scenario scenario;
    expectState(scenario.step(1), ControlState::operational, ControlAction::normal_operation);

    Readings missingBattery;
    missingBattery.battery.reset();
    const auto missing = scenario.step(2, missingBattery);
    expectState(missing, ControlState::degraded, ControlAction::limited_operation);
    expectPrimary(missing, DiagnosticCode::signal_retained, Severity::degraded, SignalId::battery_voltage);

    const auto pending = scenario.step(3);
    expectState(pending, ControlState::degraded, ControlAction::limited_operation);
    expectPrimary(pending, DiagnosticCode::recovery_pending, Severity::degraded);
    const auto skipped = scenario.step(5);
    expectState(skipped, ControlState::degraded, ControlAction::limited_operation);
    expectPrimary(skipped, DiagnosticCode::recovery_pending, Severity::degraded);
    require(!scenario.control.step(scenario.gateway.snapshot()),
            "resubmitting a healthy cycle cannot confirm recovery");
    expectState(scenario.step(6), ControlState::operational, ControlAction::normal_operation);

    Readings missingOil;
    missingOil.oil.reset();
    for (Cycle cycle = 7; cycle <= 9; ++cycle) {
        const auto retained = scenario.step(cycle, missingOil);
        expectState(retained, ControlState::degraded, ControlAction::limited_operation);
        expectPrimary(retained, DiagnosticCode::signal_retained, Severity::degraded, SignalId::oil_pressure);
    }
    const auto expired = scenario.step(10, missingOil);
    expectState(expired, ControlState::safe_state, ControlAction::inhibit_output);
    expectPrimary(expired, DiagnosticCode::signal_expired, Severity::critical, SignalId::oil_pressure);
    expectLatch(expired, 10, DiagnosticCode::signal_expired, SignalId::oil_pressure);
}

void operatingLimits()
{
    Readings values;
    values.temperature = 110.0;
    values.oil = 0.8;
    values.battery = 11.5;
    Scenario boundaries;
    expectState(boundaries.step(1, values), ControlState::operational, ControlAction::normal_operation);
    values.battery = 15.0;
    expectState(boundaries.step(2, values), ControlState::operational, ControlAction::normal_operation);
    values.battery = std::nextafter(15.0, 16.0);
    const auto voltage = boundaries.step(3, values);
    expectState(voltage, ControlState::degraded, ControlAction::limited_operation);
    expectPrimary(voltage, DiagnosticCode::battery_voltage_outside_operating_range,
                  Severity::degraded, SignalId::battery_voltage);

    values = Readings{};
    values.temperature = 130.0;
    Scenario overheating;
    const auto snapshot = overheating.publish(1, values);
    const auto measured = snapshot.observe(SignalId::coolant_temperature);
    require(measured && measured->sample && measured->sample->quality == SampleQuality::in_range,
            "130 degrees must be an accepted measurement for the independent Gateway range");
    const auto hot = overheating.control.step(snapshot);
    require(hot.has_value(), "Control must evaluate the hot reading");
    expectState(*hot, ControlState::safe_state, ControlAction::inhibit_output);
    expectPrimary(*hot, DiagnosticCode::coolant_over_temperature, Severity::critical,
                  SignalId::coolant_temperature);
    const auto measuredAfterControl = snapshot.observe(SignalId::coolant_temperature);
    require(measuredAfterControl && measuredAfterControl->sample &&
                measuredAfterControl->sample->value == 130.0,
            "Control must leave the published Gateway sample intact");

    values = Readings{};
    values.oil = std::nextafter(0.8, 0.0);
    Scenario running;
    const auto pressure = running.step(1, values);
    expectState(pressure, ControlState::safe_state, ControlAction::inhibit_output);
    expectPrimary(pressure, DiagnosticCode::low_oil_pressure, Severity::critical, SignalId::oil_pressure);

    values.rpm = 0.0;
    values.speed = 0.0;
    values.throttle = 0.0;
    values.oil = 0.0;
    Scenario stopped;
    expectState(stopped.step(1, values), ControlState::operational, ControlAction::normal_operation);
    values.rpm.reset();
    Scenario unknownEngine;
    const auto unknown = unknownEngine.step(1, values);
    expectState(unknown, ControlState::safe_state, ControlAction::inhibit_output);
    expectPrimary(unknown, DiagnosticCode::low_oil_pressure, Severity::critical, SignalId::oil_pressure);
}

void concurrentFaults()
{
    Scenario scenario;
    expectState(scenario.step(1), ControlState::operational, ControlAction::normal_operation);

    Readings mixed;
    mixed.battery.reset();
    mixed.rpm = 4500.0;
    const auto competing = scenario.step(2, mixed);
    expectState(competing, ControlState::safe_state, ControlAction::inhibit_output);
    expectDiagnostic(competing, DiagnosticCode::signal_retained, Severity::degraded, SignalId::battery_voltage);
    expectPrimary(competing, DiagnosticCode::powertrain_incoherence, Severity::critical);
    expectLatch(competing, 2, DiagnosticCode::powertrain_incoherence);

    mixed.temperature = 151.0;
    mixed.oil = std::numeric_limits<double>::infinity();
    Scenario ties;
    const auto faults = ties.step(1, mixed);
    expectPrimary(faults, DiagnosticCode::signal_out_of_range, Severity::critical, SignalId::coolant_temperature);
    expectDiagnostic(faults, DiagnosticCode::signal_non_finite, Severity::critical, SignalId::oil_pressure);
    expectDiagnostic(faults, DiagnosticCode::powertrain_incoherence, Severity::critical);

    Scenario staleRpm;
    Readings stale;
    stale.speed = 90.0;
    stale.rpm = 4500.0;
    expectState(staleRpm.step(1, stale), ControlState::operational, ControlAction::normal_operation);
    stale.speed = 60.0;
    stale.rpm.reset();
    const auto incomplete = staleRpm.step(2, stale);
    expectState(incomplete, ControlState::degraded, ControlAction::limited_operation);
    expectPrimary(incomplete, DiagnosticCode::signal_retained, Severity::degraded, SignalId::engine_rpm);
    require(!hasCode(incomplete, DiagnosticCode::powertrain_incoherence),
            "plausibility requires current speed, RPM and throttle");
}

void safeLatch()
{
    Scenario scenario;
    expectState(scenario.step(1), ControlState::operational, ControlAction::normal_operation);
    require(!scenario.control.reset(), "a running ECU must reject reset");

    Readings hot;
    hot.temperature = 130.0;
    const auto firstFault = scenario.step(2, hot);
    expectState(firstFault, ControlState::safe_state, ControlAction::inhibit_output);
    expectLatch(firstFault, 2, DiagnosticCode::coolant_over_temperature, SignalId::coolant_temperature);

    Readings lowOil;
    lowOil.oil = 0.2;
    const auto laterFault = scenario.step(3, lowOil);
    expectState(laterFault, ControlState::safe_state, ControlAction::inhibit_output);
    expectPrimary(laterFault, DiagnosticCode::low_oil_pressure, Severity::critical, SignalId::oil_pressure);
    expectLatch(laterFault, 2, DiagnosticCode::coolant_over_temperature, SignalId::coolant_temperature);

    const auto normalInput = scenario.step(4);
    expectState(normalInput, ControlState::safe_state, ControlAction::inhibit_output);
    require(normalInput.diagnostics.size() == 0, "the current inputs can recover while SAFE_STATE remains latched");
    expectLatch(normalInput, 2, DiagnosticCode::coolant_over_temperature, SignalId::coolant_temperature);
    expectLatch(firstFault, 2, DiagnosticCode::coolant_over_temperature, SignalId::coolant_temperature);

    require(scenario.control.reset(), "explicit reset must be accepted from SAFE_STATE");
    require(!scenario.control.step(scenario.gateway.snapshot()), "reset must not permit replay of the previous cycle");
    require(!scenario.control.reset(), "reset immediately clears the safe-state latch");
    const auto afterReset = scenario.step(5);
    expectState(afterReset, ControlState::operational, ControlAction::normal_operation);
    require(!afterReset.latchedFault, "the previous latch must not survive reset");

    Scenario graceRestarts;
    hot.oil.reset();
    expectState(graceRestarts.step(1, hot), ControlState::safe_state, ControlAction::inhibit_output);
    require(graceRestarts.control.reset(), "safe-state reset must be accepted");
    Readings incomplete;
    incomplete.oil.reset();
    expectState(graceRestarts.step(10, incomplete), ControlState::init, ControlAction::inhibit_output);
    expectState(graceRestarts.step(11, incomplete), ControlState::init, ControlAction::inhibit_output);
    const auto expiredGrace = graceRestarts.step(12, incomplete);
    expectState(expiredGrace, ControlState::safe_state, ControlAction::inhibit_output);
    expectLatch(expiredGrace, 12, DiagnosticCode::signal_missing, SignalId::oil_pressure);
}

void cycleOrder()
{
    Scenario scenario;
    require(!scenario.control.step(scenario.gateway.snapshot()), "cycle zero is reserved for startup");
    Readings hot;
    hot.temperature = 130.0;
    const auto previous = scenario.publish(4, hot);
    const auto latest = scenario.publish(6);
    const auto accepted = scenario.control.step(latest);
    require(accepted.has_value() && accepted->cycle == 6, "first positive cycle may have a gap");
    expectState(*accepted, ControlState::operational, ControlAction::normal_operation);
    require(!scenario.control.step(latest), "Control must reject a duplicate cycle");
    require(!scenario.control.step(previous), "Control must reject an older snapshot even if it contains a fault");
    const auto following = scenario.step(7);
    expectState(following, ControlState::operational, ControlAction::normal_operation);
    require(!following.latchedFault && following.diagnostics.size() == 0,
            "rejecting out-of-order snapshots must leave Control state untouched");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

constexpr std::array<TestCase, 6> cases{{
    {"startup", startup},
    {"degradation", degradation},
    {"operating_limits", operatingLimits},
    {"concurrent_faults", concurrentFaults},
    {"safe_latch", safeLatch},
    {"cycle_order", cycleOrder},
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
