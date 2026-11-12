#include "ecu/binary.hpp"
#include "ecu/csv.hpp"
#include "record_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ecu;
using namespace record_test;

constexpr std::size_t headerSize = 8;

std::string fromHex(std::string_view hex)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string bytes;
    require(hex.size() % 2 == 0, "reference bytes must have two digits each");
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        const auto high = digits.find(hex[index]);
        const auto low = digits.find(hex[index + 1]);
        require(high != digits.npos && low != digits.npos, "invalid hex digit in test");
        bytes.push_back(static_cast<char>(high * 16 + low));
    }
    return bytes;
}

const std::string& referenceFile()
{
    // Calculado independientemente con struct.pack y zlib.crc32.
    static const auto bytes = fromHex(
        "45435542010000005d00080706050403"
        "02010300000000000000040091020002"
        "01000707060504030201000000000000"
        "00800200000001020008070605040302"
        "01e70402000000010200080706050403"
        "0201740200000000010801ff07070605"
        "040302010801ffb41ac602");
    return bytes;
}

CycleRecord referenceRecord()
{
    CycleRecord record;
    record.cycle = 0x0102030405060708ULL;
    record.gatewayConfig.maxAgeCycles = 3;
    record.state = ControlState::safe_state;
    record.unknownId = true;
    record.duplicates[3] = true;
    record.signals[0] = {SignalSample{-0.0, SampleQuality::in_range, record.cycle - 1}, 1,
                         Freshness::retained, SignalStatus::valid};
    record.signals[2] = {SignalSample{85.5, SampleQuality::in_range, record.cycle}, 0,
                         Freshness::fresh, SignalStatus::valid};
    record.signals[4] = {SignalSample{13.8, SampleQuality::in_range, record.cycle}, 0,
                         Freshness::fresh, SignalStatus::valid};
    record.diagnosticCount = 2;
    record.diagnostics[0] = {DiagnosticCode::signal_missing, Severity::degraded, SignalId::engine_rpm};
    record.diagnostics[1] = {DiagnosticCode::powertrain_incoherence, Severity::critical, std::nullopt};
    record.latchedFault = LatchedFault{record.cycle - 1, record.diagnostics[1]};
    return record;
}

std::string writeRecords(std::initializer_list<CycleRecord> records)
{
    std::ostringstream output(std::ios::binary);
    BinaryWriter writer(output);
    for (const auto& record : records) {
        require(writer.write(record), "record must be writable");
    }
    require(writer.finish() && writer.finish() && writer.error() == BinaryError::none,
            "finish must flush the stream and be idempotent");
    return output.str();
}

void referenceBytes()
{
    const auto expected = referenceRecord();
    require(writeRecords({expected}) == referenceFile(), "written bytes differ from the reference");
    std::istringstream input(referenceFile(), std::ios::binary);
    input.exceptions(std::ios::badbit | std::ios::failbit | std::ios::eofbit);
    BinaryReader reader(input);
    CycleRecord decoded;
    require(reader.read(decoded) == BinaryReadStatus::record && reader.offset() == headerSize,
            "independent binary fixture must decode");
    expectRecord(decoded, expected);
    require(reader.read(decoded) == BinaryReadStatus::end && reader.read(decoded) == BinaryReadStatus::end &&
                reader.offset() == referenceFile().size() && reader.error() == BinaryError::none,
            "EOF between records must be clean even with stream exceptions");
    expectRecord(decoded, expected);
}

void roundTrip()
{
    constexpr auto maxCycle = std::numeric_limits<Cycle>::max();
    const CycleRecord first;
    auto last = referenceRecord();
    last.cycle = maxCycle;
    last.gatewayConfig.maxAgeCycles = maxCycle;
    last.diagnosticCount = Diagnostics::capacity;
    last.duplicates.fill(true);
    for (auto& diagnostic : last.diagnostics) {
        diagnostic = {DiagnosticCode::recovery_pending, Severity::degraded, SignalId::oil_pressure};
    }
    constexpr std::uint64_t nanBits = 0x7ff8000000004321ULL;
    double nan = 0.0;
    std::memcpy(&nan, &nanBits, sizeof(nan));
    const double infinity = std::numeric_limits<double>::infinity();
    last.signals = {{
        {SignalSample{-infinity, SampleQuality::non_finite, maxCycle}, 0, Freshness::fresh, SignalStatus::invalid},
        {SignalSample{1234.5678912345678, SampleQuality::in_range, 1}, maxCycle - 1,
         Freshness::retained, SignalStatus::valid},
        {SignalSample{85.531, SampleQuality::in_range, maxCycle}, 0, Freshness::fresh, SignalStatus::valid},
        {SignalSample{nan, SampleQuality::non_finite, maxCycle}, 0, Freshness::fresh, SignalStatus::invalid},
        {SignalSample{30.125, SampleQuality::out_of_range, maxCycle}, 0, Freshness::fresh, SignalStatus::invalid},
        {SignalSample{infinity, SampleQuality::non_finite, maxCycle}, 0, Freshness::fresh, SignalStatus::invalid},
    }};
    const auto bytes = writeRecords({first, last});
    constexpr std::size_t lastOffset = headerSize + 2 + 38 + 4;
    require(bytes.size() == lastOffset + 2 + 172 + 4 && bytes.substr(lastOffset, 2) == fromHex("ac00"),
            "maximum payload must fit, using binary64 for non-exact and out-of-scale values");
    std::istringstream input(bytes, std::ios::binary);
    BinaryReader reader(input);
    CycleRecord decoded;
    require(reader.read(decoded) == BinaryReadStatus::record, "first cycle zero must be accepted");
    expectRecord(decoded, first);
    require(reader.read(decoded) == BinaryReadStatus::record && reader.offset() == lastOffset,
            "maximum cycle and payload must be accepted");
    expectRecord(decoded, last);
    std::uint64_t decodedNan = 0;
    std::memcpy(&decodedNan, &decoded.signals[3].sample->value, sizeof(decodedNan));
    require(decodedNan == nanBits, "NaN payload bits must survive");
    require(reader.read(decoded) == BinaryReadStatus::end, "expected clean end after second record");
}

void expectReadError(const std::string& bytes, BinaryError error, std::size_t offset, bool hasPrefix = false)
{
    std::istringstream input(bytes, std::ios::binary);
    BinaryReader reader(input);
    CycleRecord decoded;
    if (hasPrefix) {
        require(reader.read(decoded) == BinaryReadStatus::record, "valid prefix must remain readable");
        expectRecord(decoded, referenceRecord());
    }
    const auto previous = decoded;
    require(reader.read(decoded) == BinaryReadStatus::error && reader.error() == error && reader.offset() == offset,
            "unexpected binary read error or offset");
    require(reader.read(decoded) == BinaryReadStatus::error && reader.error() == error && reader.offset() == offset,
            "reader failure must be sticky");
    expectRecord(decoded, previous);
}

void malformed()
{
    const auto& fixture = referenceFile();
    expectReadError("", BinaryError::invalid_header, 0);
    for (std::size_t size = 1; size < fixture.size(); ++size) {
        if (size != headerSize) {
            expectReadError(fixture.substr(0, size), BinaryError::truncated, size < headerSize ? 0 : headerSize);
        }
    }

    const auto mutate = [&](std::size_t offset, char value, BinaryError error, std::size_t frameOffset) {
        auto changed = fixture;
        changed[offset] = value;
        expectReadError(changed, error, frameOffset);
    };
    mutate(0, 'X', BinaryError::invalid_header, 0);
    mutate(4, 2, BinaryError::unsupported_version, 0);
    mutate(5, 1, BinaryError::unsupported_version, 0);
    mutate(7, 1, BinaryError::invalid_header, 0);
    mutate(8, 0, BinaryError::invalid_length, headerSize);
    mutate(8, static_cast<char>(173), BinaryError::invalid_length, headerSize);
    mutate(8, 92, BinaryError::checksum_mismatch, headerSize);
    mutate(10, 1, BinaryError::checksum_mismatch, headerSize);
    mutate(fixture.size() - 1, 0, BinaryError::checksum_mismatch, headerSize);

    // Se recalculó el CRC de cada mutación con zlib.crc32 para alcanzar la validación del contenido.
    struct Mutation {
        std::size_t offset;
        std::uint8_t value;
        std::string_view checksum;
    };
    for (const auto& mutation : {
             Mutation{26, 255, "42300289"},  // Estado desconocido.
             Mutation{27, 255, "d5c8cd2c"},  // Acción desconocida.
             Mutation{28, 17, "3969d99c"},   // Sobran los bytes del enclavamiento.
             Mutation{29, 8, "9fcd9340"},    // Capacidad de diagnósticos excedida.
             Mutation{31, 1, "49e8aa2b"},    // Frescura incompatible con la edad.
             Mutation{32, 2, "cd7ffdb7"},    // Tipo escalado en velocidad.
             Mutation{33, 255, "8624c8cf"},  // Calidad desconocida.
             Mutation{34, 9, "dae5d48f"},    // Ciclo de recepción futuro.
             Mutation{91, 254, "db566399"},  // Señal desconocida en diagnóstico.
             Mutation{100, 255, "e17f4db2"}, // Causa desconocida en enclavamiento.
         }) {
        auto changed = fixture;
        changed[mutation.offset] = static_cast<char>(mutation.value);
        changed.replace(changed.size() - 4, 4, fromHex(mutation.checksum));
        expectReadError(changed, BinaryError::invalid_record, headerSize);
    }

    const auto frame = fixture.substr(headerSize);
    expectReadError(fixture + frame, BinaryError::cycle_order, fixture.size(), true);
    expectReadError(fixture + frame.substr(0, frame.size() - 1), BinaryError::truncated, fixture.size(), true);
    auto corruptFrame = frame;
    corruptFrame.back() = 0;
    expectReadError(fixture + corruptFrame, BinaryError::checksum_mismatch, fixture.size(), true);
}

void streamErrors()
{
    const auto first = referenceRecord();
    CycleRecord second;
    second.cycle = first.cycle + 1;
    const auto& fixture = referenceFile();
    LimitedOutput buffer(fixture.size() + 9);
    std::ostream output(&buffer);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    BinaryWriter writer(output);
    require(writer.write(first) && !writer.write(second) && writer.error() == BinaryError::io_error &&
                !writer.write(second) && !writer.finish(),
            "a partial write must fail permanently");
    expectReadError(buffer.bytes, BinaryError::truncated, fixture.size(), true);

    LimitedOutput flushBuffer(256);
    std::ostream flushOutput(&flushBuffer);
    BinaryWriter flushWriter(flushOutput);
    require(flushWriter.write(first), "record must reach the stream before flush");
    flushBuffer.failFlush = true;
    require(!flushWriter.finish() && flushWriter.error() == BinaryError::io_error, "flush errors must be reported");

    std::ostringstream invalidOutput;
    BinaryWriter invalidWriter(invalidOutput);
    auto invalid = first;
    invalid.signals[0].age = 0;
    require(!invalidWriter.write(invalid) && invalidWriter.error() == BinaryError::invalid_record &&
                invalidOutput.str().empty(), "invalid records must be rejected before writing any bytes");

    for (const auto cycle : {first.cycle, first.cycle - 1}) {
        std::ostringstream orderedOutput;
        BinaryWriter orderedWriter(orderedOutput);
        CycleRecord unordered;
        unordered.cycle = cycle;
        require(orderedWriter.write(first) && !orderedWriter.write(unordered) &&
                    orderedWriter.error() == BinaryError::cycle_order && orderedOutput.str() == fixture,
                "repeated and decreasing cycles must leave the stream intact");
    }

    std::ostringstream emptyOutput;
    BinaryWriter emptyWriter(emptyOutput);
    require(emptyWriter.finish() && emptyOutput.str() == fixture.substr(0, headerSize) &&
                !emptyWriter.write(first) && emptyWriter.error() == BinaryError::closed,
            "an empty log must have a header and reject writes after finish");
    std::istringstream emptyInput(emptyOutput.str());
    BinaryReader emptyReader(emptyInput);
    CycleRecord decoded;
    require(emptyReader.read(decoded) == BinaryReadStatus::end && emptyReader.offset() == headerSize,
            "a header-only file is a valid empty log");

    std::istringstream brokenInput(fixture);
    brokenInput.setstate(std::ios::badbit);
    BinaryReader brokenReader(brokenInput);
    require(brokenReader.read(decoded) == BinaryReadStatus::error && brokenReader.error() == BinaryError::io_error,
            "input errors must stay distinct from truncated data and EOF");
}

void expectRead(std::istream& input, const std::vector<CycleRecord>& expected, CsvReader* csv = nullptr)
{
    BinaryReader reader(input);
    CycleRecord decoded;
    CycleRecord text;
    for (const auto& record : expected) {
        require(reader.read(decoded) == BinaryReadStatus::record, "expected a binary record");
        expectRecord(decoded, record);
        if (csv) {
            require(csv->read(text) == CsvReadStatus::record, "expected a CSV record");
            expectRecord(decoded, text);
        }
    }
    require(reader.read(decoded) == BinaryReadStatus::end && (!csv || csv->read(text) == CsvReadStatus::end),
            "expected the end of both logs");
}

void scenarioRoundTrip()
{
    for (const auto kind : {ScenarioKind::nominal, ScenarioKind::faults, ScenarioKind::battery_fault,
                           ScenarioKind::oil_fault, ScenarioKind::temperature_dropout}) {
        const auto scenario = makeScenario(kind);
        require(scenario.has_value(), "scenario expected");
        std::ostringstream binaryOutput(std::ios::binary);
        std::ostringstream csvOutput;
        BinaryWriter binary(binaryOutput);
        CsvWriter csv(csvOutput);
        const auto toBinary = recordObserver(binary);
        const auto toCsv = recordObserver(csv);
        require(runScenario(*scenario, [&](const SignalSnapshot& snapshot, const ControlResult& result) {
                    return toBinary(snapshot, result) && toCsv(snapshot, result);
                }) && binary.finish() && csv.finish(), "both observers must log the full scenario");
        std::istringstream binaryInput(binaryOutput.str(), std::ios::binary);
        std::istringstream csvInput(csvOutput.str());
        CsvReader reader(csvInput);
        expectRead(binaryInput, capture(kind), &reader);
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 3 && std::string_view(argv[1]) == "cli_file") {
            std::ifstream input(argv[2], std::ios::binary);
            require(static_cast<bool>(input), "the CLI output file must exist");
            expectRead(input, capture(ScenarioKind::faults));
            return EXIT_SUCCESS;
        }
        require(argc == 2, "expected a binary test name");
        struct TestCase { std::string_view name; void (*run)(); };
        const std::array cases{
            TestCase{"reference_bytes", referenceBytes}, TestCase{"round_trip", roundTrip},
            TestCase{"malformed", malformed}, TestCase{"stream_errors", streamErrors},
            TestCase{"scenarios", scenarioRoundTrip},
        };
        for (const auto& test : cases) {
            if (test.name == argv[1]) {
                test.run();
                return EXIT_SUCCESS;
            }
        }
        throw std::runtime_error("unknown binary test name");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
