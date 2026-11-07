#include "ecu/gateway.hpp"
#include "ecu/scaling.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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

double floatingTolerance(double value)
{
    return 32.0 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::fabs(value));
}

void expectNear(double actual, double expected)
{
    require(std::isfinite(actual) && std::fabs(actual - expected) <= floatingTolerance(expected),
            "decoded physical value differs from the independent expected value");
}

void expectEncoding(double physical, const LinearScale& scale,
                    std::uint16_t raw, Saturation saturation)
{
    const auto encoded = encodePhysical(physical, scale);
    require(encoded && encoded->raw == raw && encoded->saturation == saturation,
            "unexpected raw value or saturation result");
}

void profiles()
{
    struct Profile {
        SignalId id;
        LinearScale scale;
        double factor;
        double offset;
        std::uint8_t bits;
        std::uint16_t maximumRaw;
        double minimumPhysical;
        double maximumPhysical;
    };
    const std::array<Profile, 2> expected{{
        {SignalId::coolant_temperature, coolantTemperatureScale, 0.1, -40.0, 16, 65535, -40.0, 6513.5},
        {SignalId::battery_voltage, batteryVoltageScale, 0.05, 8.0, 8, 255, 8.0, 20.75},
    }};
    for (const auto& profile : expected) {
        const auto found = scalingFor(profile.id);
        require(found && found->factor == profile.factor && found->offset == profile.offset &&
                    found->rawBits == profile.bits && profile.scale.factor == profile.factor &&
                    profile.scale.offset == profile.offset && profile.scale.rawBits == profile.bits,
                "the signal profile must expose its documented factor, offset and width");
        const auto range = representableRange(*found);
        require(range && range->rawMaximum == profile.maximumRaw,
                "the raw maximum must follow the declared unsigned width");
        expectNear(range->minimum, profile.minimumPhysical);
        expectNear(range->maximum, profile.maximumPhysical);
        const auto lower = decodeRaw(0, profile.scale);
        const auto upper = decodeRaw(profile.maximumRaw, profile.scale);
        require(lower && upper, "both representable raw endpoints must decode");
        expectNear(*lower, profile.minimumPhysical);
        expectNear(*upper, profile.maximumPhysical);
        expectEncoding(profile.minimumPhysical, profile.scale, 0, Saturation::none);
        expectEncoding(profile.maximumPhysical, profile.scale, profile.maximumRaw, Saturation::none);
    }

    struct KnownValue {
        LinearScale scale;
        std::uint16_t raw;
        double physical;
    };
    const std::array<KnownValue, 6> known{{
        {coolantTemperatureScale, 1255, 85.5},
        {coolantTemperatureScale, 1300, 90.0},
        {coolantTemperatureScale, 1900, 150.0},
        {batteryVoltageScale, 116, 13.8},
        {batteryVoltageScale, 160, 16.0},
        {batteryVoltageScale, 240, 20.0},
    }};
    for (const auto& value : known) {
        const auto decoded = decodeRaw(value.raw, value.scale);
        require(decoded.has_value(), "a known raw vector must decode");
        expectNear(*decoded, value.physical);
        expectEncoding(value.physical, value.scale, value.raw, Saturation::none);
    }
    for (const auto id : {SignalId::speed, SignalId::engine_rpm, SignalId::throttle_position,
                          SignalId::oil_pressure, static_cast<SignalId>(255)}) {
        require(!scalingFor(id), "unsupported or unknown signals must not silently select a scaling profile");
    }

    const LinearScale smallestWidth{1.0, 0.0, 1};
    const auto oneBit = representableRange(smallestWidth);
    require(oneBit && oneBit->rawMaximum == 1 && oneBit->minimum == 0.0 && oneBit->maximum == 1.0,
            "the smallest supported raw width must have exactly two unsigned codes");
}

void rounding()
{
    const LinearScale integral{1.0, 0.0, 16};
    struct Expected {
        double physical;
        std::uint16_t raw;
    };
    const std::array<Expected, 9> cases{{
        {0.0, 0}, {0.25, 0}, {std::nextafter(0.5, 0.0), 0},
        {0.5, 1}, {std::nextafter(0.5, 1.0), 1},
        {1.5, 2}, {2.5, 3}, {65534.5, 65535}, {65535.0, 65535},
    }};
    for (const auto& test : cases) {
        expectEncoding(test.physical, integral, test.raw, Saturation::none);
    }

    // Estos valores producen mitades exactas en el crudo calculado con double.
    expectEncoding(-39.75, coolantTemperatureScale, 3, Saturation::none);
    expectEncoding(8.125, batteryVoltageScale, 3, Saturation::none);
    const LinearScale descending{-0.5, 10.0, 8};
    expectEncoding(9.75, descending, 1, Saturation::none);
    expectEncoding(9.25, descending, 2, Saturation::none);
}

void saturation()
{
    const double maximum = std::numeric_limits<double>::max();
    const double infinity = std::numeric_limits<double>::infinity();
    for (const auto& scale : {coolantTemperatureScale, batteryVoltageScale}) {
        const auto range = representableRange(scale);
        require(range.has_value(), "the established profiles must have a physical range");
        expectEncoding(range->minimum, scale, 0, Saturation::none);
        expectEncoding(range->maximum, scale, range->rawMaximum, Saturation::none);
        expectEncoding(std::nextafter(range->minimum, -infinity), scale, 0, Saturation::below_range);
        expectEncoding(std::nextafter(range->maximum, infinity), scale,
                       range->rawMaximum, Saturation::above_range);
        expectEncoding(-maximum, scale, 0, Saturation::below_range);
        expectEncoding(maximum, scale, range->rawMaximum, Saturation::above_range);
    }

    const LinearScale integral{1.0, 0.0, 16};
    expectEncoding(-0.25, integral, 0, Saturation::below_range);
    expectEncoding(65535.25, integral, 65535, Saturation::above_range);

    const LinearScale descending{-2.0, 100.0, 8};
    const auto range = representableRange(descending);
    require(range && range->minimum == -410.0 && range->maximum == 100.0 && range->rawMaximum == 255,
            "a negative factor must reverse physical endpoints without changing the raw width");
    expectEncoding(-410.0, descending, 255, Saturation::none);
    expectEncoding(100.0, descending, 0, Saturation::none);
    expectEncoding(-1000.0, descending, 255, Saturation::below_range);
    expectEncoding(1000.0, descending, 0, Saturation::above_range);
    const auto decoded = decodeRaw(255, descending);
    require(decoded && *decoded == -410.0, "a negative factor must also decode the upper raw endpoint");
}

void invalidInput()
{
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double maximum = std::numeric_limits<double>::max();
    for (const auto& scale : {coolantTemperatureScale, batteryVoltageScale}) {
        for (const double invalid : {nan, infinity, -infinity}) {
            require(!encodePhysical(invalid, scale), "non-finite physical values must be rejected instead of saturated");
        }
    }
    const std::array<LinearScale, 13> invalidScales{{
        {0.0, 0.0, 16}, {-0.0, 0.0, 16},
        {nan, 0.0, 16}, {infinity, 0.0, 16}, {-infinity, 0.0, 16},
        {1.0, nan, 16}, {1.0, infinity, 16}, {1.0, -infinity, 16},
        {1.0, 0.0, 0}, {1.0, 0.0, 17}, {1.0, 0.0, 255},
        {maximum, 0.0, 16}, {std::numeric_limits<double>::denorm_min(), 1.0, 16},
    }};
    for (const auto& scale : invalidScales) {
        require(!representableRange(scale) && !encodePhysical(1.0, scale) && !decodeRaw(0, scale),
                "an invalid scale must reject range queries and both conversion directions");
    }
    require(!decodeRaw(256, batteryVoltageScale) && !decodeRaw(65536, coolantTemperatureScale) &&
                !decodeRaw(std::numeric_limits<std::uint32_t>::max(), coolantTemperatureScale),
            "decoding must reject raw values outside the declared width before narrowing");

    const LinearScale widePhysical{maximum / 2.0, -maximum, 2};
    const auto range = representableRange(widePhysical);
    require(range && std::isfinite(range->minimum) && std::isfinite(range->maximum),
            "a scale with finite combined endpoints must remain usable");
    expectEncoding(maximum / 4.0, widePhysical, 3, Saturation::none);
}

void roundTrip()
{
    for (const auto& scale : {coolantTemperatureScale, batteryVoltageScale}) {
        const auto range = representableRange(scale);
        require(range.has_value(), "every established profile must have a range");
        for (std::uint32_t raw = 0; raw <= range->rawMaximum; ++raw) {
            const auto physical = decodeRaw(raw, scale);
            require(physical && std::isfinite(*physical), "every representable raw code must decode to a finite value");
            const auto encoded = encodePhysical(*physical, scale);
            require(encoded && encoded->raw == raw && encoded->saturation == Saturation::none,
                    "every raw code of the established profiles must survive decode and encode exactly");
        }
    }

    const std::array<double, 10> temperatures{
        -40.0, -39.99, -39.95, -0.03, 25.06, 85.55, 90.04, 149.97, 6513.49, 6513.5,
    };
    const std::array<double, 10> voltages{
        8.0, 8.001, 8.025, 9.0, 11.54, 13.825, 15.99, 16.0, 20.7499, 20.75,
    };
    const auto verifyQuantization = [](double physical, const LinearScale& scale) {
        const auto encoded = encodePhysical(physical, scale);
        require(encoded && encoded->saturation == Saturation::none,
                "a representable physical sample must encode without saturation");
        const auto decoded = decodeRaw(encoded->raw, scale);
        require(decoded && std::fabs(*decoded - physical) <=
                    std::fabs(scale.factor) / 2.0 + floatingTolerance(physical),
                "the physical round trip must stay within half a raw step plus floating roundoff");
    };
    for (const auto physical : temperatures) {
        verifyQuantization(physical, coolantTemperatureScale);
    }
    for (const auto physical : voltages) {
        verifyQuantization(physical, batteryVoltageScale);
    }
}

void observations()
{
    Gateway gateway;
    SignalBatch batch;
    batch.add(SignalId::coolant_temperature, -100.0);
    batch.add(SignalId::battery_voltage, 30.0);
    const auto snapshot = gateway.processCycle(10, batch);
    require(snapshot.has_value(), "the source measurements must reach Gateway");
    const auto temperature = snapshot->observe(SignalId::coolant_temperature);
    const auto battery = snapshot->observe(SignalId::battery_voltage);
    require(temperature && temperature->sample && battery && battery->sample &&
                temperature->status == SignalStatus::invalid && battery->status == SignalStatus::invalid,
            "the physical source values must retain their independent Gateway classification");
    expectEncoding(temperature->sample->value, coolantTemperatureScale, 0, Saturation::below_range);
    expectEncoding(battery->sample->value, batteryVoltageScale, 255, Saturation::above_range);
    const auto bounded = decodeRaw(0, coolantTemperatureScale);
    require(bounded && *bounded == -40.0,
            "a saturated raw zero may decode to a physical value accepted by the signal catalog");

    const auto unchangedTemperature = snapshot->observe(SignalId::coolant_temperature);
    const auto unchangedBattery = gateway.snapshot().observe(SignalId::battery_voltage);
    require(unchangedTemperature && unchangedTemperature->sample && unchangedBattery && unchangedBattery->sample &&
                unchangedTemperature->sample->value == -100.0 && unchangedBattery->sample->value == 30.0 &&
                unchangedTemperature->sample->quality == SampleQuality::out_of_range &&
                unchangedBattery->sample->quality == SampleQuality::out_of_range &&
                unchangedTemperature->status == SignalStatus::invalid &&
                unchangedBattery->status == SignalStatus::invalid && unchangedTemperature->age == 0 &&
                unchangedBattery->age == 0 && unchangedTemperature->sample->receivedAt == 10 &&
                unchangedBattery->sample->receivedAt == 10,
            "scaling must not overwrite source samples, reception cycles, quality or Gateway results");

    const auto expired = gateway.processCycle(14, SignalBatch{});
    require(expired.has_value(), "an omitted sample must age normally");
    const auto old = expired->observe(SignalId::coolant_temperature);
    require(old && old->sample && old->status == SignalStatus::missing && old->age == 4,
            "a retained source value may belong to a MISSING observation");
    expectEncoding(old->sample->value, coolantTemperatureScale, 0, Saturation::below_range);
    require(expired->observe(SignalId::coolant_temperature)->status == SignalStatus::missing,
            "encoding the last value cannot recover the freshness of a signal");

    SignalBatch nonfinite;
    nonfinite.add(SignalId::coolant_temperature, std::numeric_limits<double>::quiet_NaN());
    const auto invalid = gateway.processCycle(15, nonfinite);
    require(invalid.has_value(), "a non-finite physical sample must remain available to Gateway diagnostics");
    const auto rawSource = invalid->observe(SignalId::coolant_temperature);
    require(rawSource && rawSource->sample && !encodePhysical(rawSource->sample->value, coolantTemperatureScale),
            "a non-finite source value must be rejected by scaling");
    const auto afterRejection = invalid->observe(SignalId::coolant_temperature);
    require(afterRejection && afterRejection->sample && std::isnan(afterRejection->sample->value) &&
                afterRejection->status == SignalStatus::invalid &&
                afterRejection->sample->quality == SampleQuality::non_finite,
            "a rejected conversion must leave the original non-finite observation intact");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

constexpr std::array<TestCase, 6> cases{{
    {"profiles", profiles},
    {"rounding", rounding},
    {"saturation", saturation},
    {"invalid_input", invalidInput},
    {"round_trip", roundTrip},
    {"observations", observations},
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
