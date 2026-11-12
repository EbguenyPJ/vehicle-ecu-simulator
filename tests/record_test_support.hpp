#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

#include "ecu/record.hpp"
#include "ecu/simulation.hpp"

namespace record_test {

using namespace ecu;

inline void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

inline bool sameDiagnostic(const Diagnostic& a, const Diagnostic& b)
{
    return a.code == b.code && a.severity == b.severity && a.signal == b.signal;
}

inline void expectObservation(const SignalObservation& actual, const SignalObservation& expected)
{
    require(actual.status == expected.status && actual.freshness == expected.freshness &&
                actual.age == expected.age && actual.sample.has_value() == expected.sample.has_value(),
            "observation metadata changed");
    if (expected.sample) {
        const auto a = actual.sample->value;
        const auto b = expected.sample->value;
        const bool same = std::isnan(b) ? std::isnan(a) : a == b && std::signbit(a) == std::signbit(b);
        require(same && actual.sample->receivedAt == expected.sample->receivedAt &&
                    actual.sample->quality == expected.sample->quality,
                "physical sample or reception metadata changed");
    }
}

inline void expectRecord(const CycleRecord& actual, const CycleRecord& expected)
{
    require(actual.cycle == expected.cycle && actual.gatewayConfig.maxAgeCycles == expected.gatewayConfig.maxAgeCycles &&
                actual.state == expected.state && actual.action == expected.action &&
                actual.unknownId == expected.unknownId && actual.duplicates == expected.duplicates &&
                actual.diagnosticCount == expected.diagnosticCount &&
                actual.latchedFault.has_value() == expected.latchedFault.has_value(),
            "cycle, Control result or input diagnostics changed");
    for (std::size_t index = 0; index < signalCount; ++index) {
        expectObservation(actual.signals[index], expected.signals[index]);
    }
    for (std::size_t index = 0; index < expected.diagnosticCount; ++index) {
        require(sameDiagnostic(actual.diagnostics[index], expected.diagnostics[index]),
                "a current diagnostic or its order changed");
    }
    if (expected.latchedFault) {
        require(actual.latchedFault->cycle == expected.latchedFault->cycle &&
                    sameDiagnostic(actual.latchedFault->cause, expected.latchedFault->cause),
                "the latched fault changed");
    }
}

struct MemorySink {
    std::size_t failAfter{std::numeric_limits<std::size_t>::max()};
    std::size_t calls{0};
    std::vector<CycleRecord> records;

    bool write(const CycleRecord& record)
    {
        ++calls;
        if (records.size() == failAfter) {
            return false;
        }
        records.push_back(record);
        return true;
    }
};

inline std::vector<CycleRecord> capture(ScenarioKind kind)
{
    const auto scenario = makeScenario(kind);
    MemorySink sink;
    require(scenario && runScenario(*scenario, recordObserver(sink)), "the capture must complete");
    require(sink.records.size() == scenario->cycles(), "one record per cycle is required");
    return sink.records;
}

class LimitedOutput : public std::streambuf {
public:
    explicit LimitedOutput(std::size_t limit) : limit_(limit) {}
    std::string bytes;
    bool failFlush{false};

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override
    {
        const auto accepted = std::min(static_cast<std::size_t>(count), limit_ - bytes.size());
        bytes.append(data, accepted);
        return static_cast<std::streamsize>(accepted);
    }

    int_type overflow(int_type value) override
    {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        const char ch = traits_type::to_char_type(value);
        return xsputn(&ch, 1) == 1 ? value : traits_type::eof();
    }

    int sync() override { return failFlush ? -1 : 0; }

private:
    std::size_t limit_;
};

}  // namespace record_test
