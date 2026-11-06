#include "ecu/gateway.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
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

SignalSnapshot process(Gateway& gateway, Cycle cycle, const SignalBatch& batch)
{
    const auto snapshot = gateway.processCycle(cycle, batch);
    require(snapshot.has_value(), "a strictly increasing cycle must be accepted");
    require(snapshot->cycle() == cycle, "the snapshot must identify the accepted cycle");
    return *snapshot;
}

SignalObservation observe(const SignalSnapshot& snapshot, SignalId id)
{
    const auto observation = snapshot.observe(id);
    require(observation.has_value(), "every catalog signal must have an observation");
    return *observation;
}

void expectSample(const SignalSnapshot& snapshot, SignalId id,
                  SignalSample sample, Cycle age, Freshness freshness)
{
    const auto observation = observe(snapshot, id);
    require(observation.sample.has_value(), "a received sample must be present");
    require(observation.freshness == freshness, "unexpected sample freshness");
    require(observation.age == age, "unexpected sample age");
    require(observation.sample->receivedAt == sample.receivedAt, "unexpected reception cycle");
    require(observation.sample->quality == sample.quality, "unexpected sample quality");
    const double actual = observation.sample->value;
    const bool sameValue = std::isnan(sample.value) ? std::isnan(actual) : actual == sample.value;
    require(sameValue, "the sample must preserve the received value");
}

void expectNeverReceived(const SignalSnapshot& snapshot, SignalId id)
{
    const auto observation = observe(snapshot, id);
    require(observation.freshness == Freshness::never_received, "unexpected initial freshness");
    require(!observation.sample && !observation.age, "absence must have neither sample nor age");
}

void catalogAndRange()
{
    struct ExpectedSignal {
        SignalId id;
        std::string_view unit;
        double minimum;
        double maximum;
    };
    const std::array<ExpectedSignal, 6> expected{{
        {SignalId::speed, "km/h", 0.0, 250.0},
        {SignalId::engine_rpm, "rpm", 0.0, 8000.0},
        {SignalId::coolant_temperature, "degC", -40.0, 150.0},
        {SignalId::throttle_position, "%", 0.0, 100.0},
        {SignalId::battery_voltage, "V", 9.0, 16.0},
        {SignalId::oil_pressure, "bar", 0.0, 10.0},
    }};
    require(signalCatalog.size() == expected.size(), "the catalog must contain six signals");

    Gateway gateway;
    Cycle cycle = 0;
    const double infinity = std::numeric_limits<double>::infinity();
    for (const auto& signal : expected) {
        const auto descriptor = std::find_if(signalCatalog.begin(), signalCatalog.end(),
                                             [&signal](const auto& item) {
                                                 return item.id == signal.id;
                                             });
        require(descriptor != signalCatalog.end(), "missing catalog ID");
        require(descriptor->unit == signal.unit, "unexpected catalog unit");
        require(descriptor->minimum == signal.minimum && descriptor->maximum == signal.maximum,
                "unexpected catalog range");
        require(descriptor->toleratedMissingCycles == 3, "unexpected catalog timeout");

        const auto checkReading = [&](double value, SampleQuality quality) {
            SignalBatch batch;
            require(batch.add(signal.id, value) == InsertResult::inserted, "catalog ID must be accepted");
            const auto snapshot = process(gateway, ++cycle, batch);
            expectSample(snapshot, signal.id, {value, quality, cycle}, 0, Freshness::fresh);
        };
        checkReading(signal.minimum, SampleQuality::in_range);
        checkReading(signal.maximum, SampleQuality::in_range);
        checkReading(std::nextafter(signal.minimum, -infinity), SampleQuality::out_of_range);
        checkReading(std::nextafter(signal.maximum, infinity), SampleQuality::out_of_range);
        checkReading(std::numeric_limits<double>::quiet_NaN(), SampleQuality::non_finite);
        checkReading(infinity, SampleQuality::non_finite);
        checkReading(-infinity, SampleQuality::non_finite);
    }
}

void freshness()
{
    Gateway gateway;
    const auto initial = gateway.snapshot();
    require(initial.cycle() == 0, "the initial snapshot must be cycle zero");
    for (const auto& descriptor : signalCatalog) {
        expectNeverReceived(initial, descriptor.id);
    }

    SignalBatch first;
    require(first.add(SignalId::speed, 0.0) == InsertResult::inserted, "zero speed is a sample");
    require(first.add(SignalId::coolant_temperature, 180.0) == InsertResult::inserted,
            "out-of-range input must reach Gateway validation");
    const auto received = process(gateway, 1, first);
    const SignalSample stopped{0.0, SampleQuality::in_range, 1};
    const SignalSample overRange{180.0, SampleQuality::out_of_range, 1};
    expectSample(received, SignalId::speed, stopped, 0, Freshness::fresh);
    expectSample(received, SignalId::coolant_temperature, overRange, 0, Freshness::fresh);

    for (Cycle age = 1; age <= 4; ++age) {
        const auto snapshot = process(gateway, 1 + age, SignalBatch{});
        const auto state = age == 4 ? Freshness::expired : Freshness::retained;
        expectSample(snapshot, SignalId::speed, stopped, age, state);
        expectSample(snapshot, SignalId::coolant_temperature, overRange, age, state);
        expectNeverReceived(snapshot, SignalId::battery_voltage);
    }

    SignalBatch recovery;
    require(recovery.add(SignalId::speed, 0.0) == InsertResult::inserted, "identical later reading is new");
    require(recovery.add(SignalId::coolant_temperature, 90.0) == InsertResult::inserted,
            "recovery reading must be inserted");
    const auto recovered = process(gateway, 6, recovery);
    expectSample(recovered, SignalId::speed, {0.0, SampleQuality::in_range, 6}, 0, Freshness::fresh);
    expectSample(recovered, SignalId::coolant_temperature, {90.0, SampleQuality::in_range, 6},
                 0, Freshness::fresh);
}

constexpr auto unknownSignal = static_cast<SignalId>(255);

void malformedBatch()
{
    for (const auto& values : std::array<std::array<double, 2>, 2>{{{60.0, 251.0}, {251.0, 60.0}}}) {
        Gateway gateway;
        SignalBatch seed;
        require(seed.add(SignalId::speed, 42.0) == InsertResult::inserted, "initial speed must be inserted");
        process(gateway, 1, seed);

        SignalBatch batch;
        require(batch.add(SignalId::speed, values[0]) == InsertResult::inserted, "first speed must be inserted");
        require(batch.add(SignalId::speed, values[1]) == InsertResult::duplicate_id, "second speed is a duplicate");
        require(batch.add(SignalId::speed, 50.0) == InsertResult::duplicate_id, "third speed remains a duplicate");
        require(batch.add(SignalId::engine_rpm, 1000.0) == InsertResult::inserted, "first RPM must be inserted");
        require(batch.add(SignalId::engine_rpm, 1200.0) == InsertResult::duplicate_id, "second RPM is a duplicate");
        require(batch.add(unknownSignal, 1.0) == InsertResult::unknown_id, "unknown IDs must be rejected");
        require(batch.add(SignalId::oil_pressure, 2.5) == InsertResult::inserted, "a different ID is independent");

        const auto snapshot = process(gateway, 2, batch);
        expectSample(snapshot, SignalId::speed, {42.0, SampleQuality::in_range, 1}, 1, Freshness::retained);
        expectNeverReceived(snapshot, SignalId::engine_rpm);
        expectSample(snapshot, SignalId::oil_pressure, {2.5, SampleQuality::in_range, 2}, 0, Freshness::fresh);
        require(snapshot.diagnostics().hasDuplicate(SignalId::speed) &&
                    snapshot.diagnostics().hasDuplicate(SignalId::engine_rpm),
                "both duplicated signals must be diagnosed");
        require(!snapshot.diagnostics().hasDuplicate(SignalId::oil_pressure), "the unique sample is not duplicated");
        require(!snapshot.diagnostics().hasDuplicate(unknownSignal), "an unknown ID has no duplicate slot");
        require(snapshot.diagnostics().hasUnknownId(), "the unknown ID must be diagnosed");
        require(!snapshot.observe(unknownSignal), "an unknown ID has no observation");

        const auto clean = process(gateway, 3, SignalBatch{});
        require(!clean.diagnostics().hasDuplicate(SignalId::speed) && !clean.diagnostics().hasUnknownId(),
                "input diagnostics must belong only to the cycle that produced them");
    }
}

void snapshotOwnership()
{
    struct Change {
        SignalId id;
        double first;
        double next;
    };
    const std::array<Change, 6> changes{{
        {SignalId::speed, 42.0, 55.0},
        {SignalId::engine_rpm, 2000.0, 2100.0},
        {SignalId::coolant_temperature, 90.0, 95.0},
        {SignalId::throttle_position, 12.0, 15.0},
        {SignalId::battery_voltage, 13.5, 13.2},
        {SignalId::oil_pressure, 2.5, 2.8},
    }};
    const auto saved = [&changes] {
        Gateway gateway;
        SignalBatch first;
        SignalBatch second;
        for (const auto& change : changes) {
            require(first.add(change.id, change.first) == InsertResult::inserted, "first batch must fit all IDs");
            require(second.add(change.id, change.next) == InsertResult::inserted, "second batch must fit all IDs");
        }
        require(first.add(unknownSignal, 1.0) == InsertResult::unknown_id, "unknown IDs must be rejected");
        const auto snapshot = process(gateway, 1, first);
        const auto next = process(gateway, 2, second);
        require(!next.diagnostics().hasUnknownId(), "the next snapshot has its own diagnostics");

        for (const auto& change : changes) {
            expectSample(next, change.id, {change.next, SampleQuality::in_range, 2}, 0, Freshness::fresh);
        }
        return snapshot;
    }();

    require(saved.cycle() == 1, "a saved snapshot must retain its cycle after Gateway destruction");
    require(saved.diagnostics().hasUnknownId(), "a saved snapshot must retain its diagnostics");
    for (const auto& change : changes) {
        expectSample(saved, change.id, {change.first, SampleQuality::in_range, 1}, 0, Freshness::fresh);
    }
}

void cycleOrder()
{
    Gateway gateway;
    SignalBatch seed;
    require(seed.add(SignalId::speed, 42.0) == InsertResult::inserted, "initial speed must be inserted");
    require(!gateway.processCycle(0, seed), "cycle zero is reserved for the initial state");
    expectNeverReceived(gateway.snapshot(), SignalId::speed);
    process(gateway, 2, seed);

    SignalBatch rejected;
    require(rejected.add(SignalId::speed, 55.0) == InsertResult::inserted, "changed speed must be inserted");
    require(rejected.add(unknownSignal, 1.0) == InsertResult::unknown_id, "unknown IDs must be rejected");
    require(!gateway.processCycle(2, rejected), "repeating a cycle must be rejected");
    require(!gateway.processCycle(1, rejected), "going backward must be rejected");
    const auto unchanged = gateway.snapshot();
    require(unchanged.cycle() == 2 && !unchanged.diagnostics().hasUnknownId(),
            "a rejected cycle must not alter published state or diagnostics");
    const SignalSample speed{42.0, SampleQuality::in_range, 2};
    expectSample(unchanged, SignalId::speed, speed, 0, Freshness::fresh);

    const auto skipped = process(gateway, 5, SignalBatch{});
    expectSample(skipped, SignalId::speed, speed, 3, Freshness::retained);
    const auto maximum = std::numeric_limits<Cycle>::max();
    const auto last = process(gateway, maximum, SignalBatch{});
    expectSample(last, SignalId::speed, speed, maximum - 2, Freshness::expired);
    require(!gateway.processCycle(maximum, rejected), "maximum cycle cannot be repeated");
    require(!gateway.processCycle(0, rejected), "cycle numbers must never wrap to zero");
    require(gateway.snapshot().cycle() == maximum, "last accepted cycle must remain visible");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

constexpr std::array<TestCase, 5> cases{{
    {"catalog_and_range", catalogAndRange},
    {"freshness", freshness},
    {"malformed_batch", malformedBatch},
    {"snapshot", snapshotOwnership},
    {"cycle_order", cycleOrder},
}};

} // namespace

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
