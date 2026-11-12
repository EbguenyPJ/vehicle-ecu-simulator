#include "ecu/binary.hpp"
#include "ecu/csv.hpp"
#include "record_test_support.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Records = std::vector<ecu::CycleRecord>;
using record_test::require;
namespace fs = std::filesystem;

constexpr std::size_t scenarioCopies = 1024;
constexpr std::size_t measuredSamples = 21;
constexpr std::size_t warmupSamples = 2;

Records repeatTrace(const Records& trace, std::size_t copies)
{
    Records records;
    records.reserve(trace.size() * copies);
    for (std::size_t copy = 0; copy < copies; ++copy) {
        const ecu::Cycle offset = static_cast<ecu::Cycle>(copy) * trace.back().cycle;
        for (auto record : trace) {
            record.cycle += offset;
            for (auto& signal : record.signals) {
                if (signal.sample) {
                    signal.sample->receivedAt += offset;
                }
            }
            if (record.latchedFault) {
                record.latchedFault->cycle += offset;
            }
            records.push_back(record);
        }
    }
    return records;
}

template <typename Writer>
std::int64_t measureWrite(const fs::path& path, const Records& records)
{
    const auto start = Clock::now();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "could not open log for writing");
    Writer writer(output);
    for (const auto& record : records) {
        require(writer.write(record), "log write failed");
    }
    require(writer.finish(), "log flush failed");
    output.close();
    require(static_cast<bool>(output), "log close after writing failed");
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

template <typename Reader, typename Status>
std::int64_t measureRead(const fs::path& path, Records& decoded)
{
    const auto start = Clock::now();
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "could not open log for reading");
    Reader reader(input);
    for (auto& record : decoded) {
        require(reader.read(record) == Status::record, "missing or invalid record");
    }
    ecu::CycleRecord extra;
    require(reader.read(extra) == Status::end, "log must end after the expected records");
    input.clear();  // El lector ya comprobó que el EOF fue limpio.
    input.close();
    require(static_cast<bool>(input), "log close after reading failed");
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

struct Measurement {
    std::uintmax_t bytes;
    std::int64_t writeNs;
    std::int64_t readNs;
};

template <typename Writer, typename Reader, typename Status>
Measurement measure(const fs::path& path, const Records& records, Records& decoded)
{
    try {
        const auto writeNs = measureWrite<Writer>(path, records);
        const auto bytes = fs::file_size(path);
        const auto readNs = measureRead<Reader, Status>(path, decoded);
        for (std::size_t index = 0; index < records.size(); ++index) {
            record_test::expectRecord(decoded[index], records[index]);
        }
        return {bytes, writeNs, readNs};
    } catch (const std::exception& error) {
        throw std::runtime_error(path.string() + ": " + error.what());
    }
}

void printSample(std::string_view format, std::size_t sample, std::size_t copies,
                 std::size_t records, const Measurement& measurement)
{
    std::cout << format << ',' << sample << ',' << copies << ',' << records << ','
              << measurement.bytes << ',' << measurement.writeNs << ',' << measurement.readNs << '\n';
}

}  // namespace

int main(int argc, char* argv[])
{
    const bool verify = argc == 3 && std::string_view(argv[2]) == "--verify";
    if (argc != 2 && !verify) {
        std::cerr << "Usage: ecu-log-performance NEW_DIRECTORY [--verify]\n";
        return EXIT_FAILURE;
    }
#ifndef NDEBUG
    if (!verify) {
        std::cerr << "Compile ecu-log-performance in Release or RelWithDebInfo.\n";
        return EXIT_FAILURE;
    }
#endif

    try {
        const fs::path directory(argv[1]);
        const auto copies = verify ? std::size_t{2} : scenarioCopies;
        const auto warmups = verify ? std::size_t{0} : warmupSamples;
        const auto samples = verify ? std::size_t{1} : measuredSamples;
        const auto trace = record_test::capture(ecu::ScenarioKind::faults);
        require(!trace.empty(), "the faults trace is empty");
        const auto records = repeatTrace(trace, copies);
        Records decoded(records.size());
        require(fs::create_directory(directory), "the output directory must be new");

        std::cout.imbue(std::locale::classic());
        std::cout << "format,sample,scenario_copies,records,bytes,write_ns,read_ns\n";
        const auto csvPath = directory / "trace.csv";
        const auto binaryPath = directory / "trace.bin";
        const auto csv = [&](std::size_t iteration) {
            const auto result = measure<ecu::CsvWriter, ecu::CsvReader, ecu::CsvReadStatus>(csvPath, records, decoded);
            if (iteration >= warmups) {
                printSample("csv", iteration - warmups + 1, copies, records.size(), result);
            }
        };
        const auto binary = [&](std::size_t iteration) {
            const auto result = measure<ecu::BinaryWriter, ecu::BinaryReader, ecu::BinaryReadStatus>(
                binaryPath, records, decoded);
            if (iteration >= warmups) {
                printSample("binary", iteration - warmups + 1, copies, records.size(), result);
            }
        };
        for (std::size_t iteration = 0; iteration < warmups + samples; ++iteration) {
            if (iteration % 2 == 0) {
                csv(iteration);
                binary(iteration);
            } else {
                binary(iteration);
                csv(iteration);
            }
        }
        return std::cout.flush() ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
