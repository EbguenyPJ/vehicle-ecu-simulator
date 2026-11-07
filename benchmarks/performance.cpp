#include "ecu/dashboard.hpp"
#include "ecu/simulation.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <locale>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t sampleCount = 21;
constexpr double minimumSampleNs = 100'000'000.0;

struct Frame {
    ecu::SignalSnapshot snapshot;
    ecu::ControlResult control;
};

class CountingBuffer final : public std::streambuf {
public:
    void reset() noexcept { bytes_ = 0; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

protected:
    std::streamsize xsputn(const char*, std::streamsize size) override
    {
        bytes_ += static_cast<std::uint64_t>(size);
        return size;
    }

    int_type overflow(int_type value) override
    {
        if (!traits_type::eq_int_type(value, traits_type::eof())) {
            ++bytes_;
        }
        return traits_type::not_eof(value);
    }

private:
    std::uint64_t bytes_{0};
};

enum class Workload {
    core,
    ascii,
    braille,
};

constexpr std::array workloads{Workload::core, Workload::ascii, Workload::braille};

std::string_view nameOf(Workload workload) noexcept
{
    switch (workload) {
    case Workload::core: return "core";
    case Workload::ascii: return "dashboard_ascii";
    case Workload::braille: return "dashboard_braille";
    }
    return "unknown";
}

struct WorkResult {
    std::uint64_t checksum;
    std::uint64_t bytes;
};

WorkResult runWork(Workload workload, std::size_t repetitions, const ecu::Scenario& scenario,
                   const std::vector<Frame>& trace, std::ostream& output, CountingBuffer& buffer)
{
    buffer.reset();
    std::uint64_t checksum = 0;

    if (workload == Workload::core) {
        const auto consume = [&checksum](const ecu::SignalSnapshot&, const ecu::ControlResult& control) {
            checksum += control.cycle + static_cast<std::uint64_t>(control.state) +
                        static_cast<std::uint64_t>(control.diagnostics.size());
        };
        for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
            if (!ecu::runScenario(scenario, consume)) {
                throw std::runtime_error("the core did not complete the scenario");
            }
        }
    } else {
        const auto format = workload == Workload::ascii
            ? ecu::DashboardFormat::ascii : ecu::DashboardFormat::braille;
        for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
            for (const auto& frame : trace) {
                if (!ecu::renderDashboard(output, frame.snapshot, frame.control, format)) {
                    throw std::runtime_error("the Dashboard could not render a recorded cycle");
                }
            }
        }
        checksum = buffer.bytes();
    }
    return {checksum, buffer.bytes()};
}

struct Measurement {
    double elapsedNs;
    WorkResult work;
};

Measurement measure(Workload workload, std::size_t repetitions, const ecu::Scenario& scenario,
                    const std::vector<Frame>& trace, std::ostream& output, CountingBuffer& buffer)
{
    const auto start = Clock::now();
    const auto work = runWork(workload, repetitions, scenario, trace, output, buffer);
    const auto end = Clock::now();
    return {std::chrono::duration<double, std::nano>(end - start).count(), work};
}

std::size_t calibrate(Workload workload, const ecu::Scenario& scenario, const std::vector<Frame>& trace,
                      std::ostream& output, CountingBuffer& buffer, std::uint64_t& checksum)
{
    // La calibración también calienta el código y las carátulas y rótulos estáticos.
    std::size_t repetitions = 1;
    for (;;) {
        const auto sample = measure(workload, repetitions, scenario, trace, output, buffer);
        checksum += sample.work.checksum;
        if (sample.elapsedNs >= minimumSampleNs) {
            return repetitions;
        }
        if (repetitions >= 1'048'576) {
            throw std::runtime_error("calibration exceeded its repetition limit");
        }
        repetitions *= 2;
    }
}

}  // namespace

int main()
{
#ifndef NDEBUG
    std::cerr << "Compile ecu-performance in Release or RelWithDebInfo.\n";
    return EXIT_FAILURE;
#endif
    try {
        const auto scenario = ecu::makeScenario(ecu::ScenarioKind::faults);
        if (!scenario) {
            throw std::runtime_error("the faults scenario is unavailable");
        }

        std::vector<Frame> trace;
        trace.reserve(static_cast<std::size_t>(scenario->cycles()));
        if (!ecu::runScenario(*scenario, [&trace](const ecu::SignalSnapshot& snapshot,
                                                const ecu::ControlResult& control) {
                trace.push_back({snapshot, control});
            }) || trace.size() != scenario->cycles()) {
            throw std::runtime_error("the recorded trace is incomplete");
        }

        CountingBuffer buffer;
        std::ostream output(&buffer);
        std::array<std::size_t, workloads.size()> repetitions{};
        std::uint64_t checksum = 0;
        for (std::size_t index = 0; index < workloads.size(); ++index) {
            repetitions[index] = calibrate(workloads[index], *scenario, trace, output, buffer, checksum);
        }

        std::cout.imbue(std::locale::classic());
        std::cout << "case,sample,scenario_repetitions,cycles,elapsed_ns,ns_per_cycle,bytes_per_cycle\n"
                  << std::fixed << std::setprecision(3);
        for (std::size_t sample = 1; sample <= sampleCount; ++sample) {
            for (std::size_t offset = 0; offset < workloads.size(); ++offset) {
                const auto index = (sample - 1 + offset) % workloads.size();
                const auto count = repetitions[index];
                const auto measured = measure(workloads[index], count, *scenario, trace, output, buffer);
                checksum += measured.work.checksum;
                const auto cycles = count * trace.size();
                const auto cycleCount = static_cast<double>(cycles);
                std::cout << nameOf(workloads[index]) << ',' << sample << ',' << count << ',' << cycles << ','
                          << measured.elapsedNs << ',' << measured.elapsedNs / cycleCount << ','
                          << static_cast<double>(measured.work.bytes) / cycleCount << '\n';
            }
        }

        std::cerr << "scenario=faults cycles=" << trace.size()
                  << " samples=" << sampleCount << " checksum=" << checksum << '\n';
        if (!std::cout) {
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
