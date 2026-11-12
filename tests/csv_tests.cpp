#include "ecu/csv.hpp"
#include "record_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ecu;

using namespace record_test;

void recordCapture()
{
    const auto saved = [] {
        Gateway gateway{GatewayConfig{1}};
        ControlECU control;
        SignalBatch batch;
        batch.add(SignalId::coolant_temperature, 85.5);
        batch.add(SignalId::oil_pressure, -1.0);
        batch.add(SignalId::battery_voltage, 12.0);
        batch.add(SignalId::battery_voltage, 13.0);
        batch.add(static_cast<SignalId>(255), 42.0);
        const auto snapshot = gateway.processCycle(7, batch);
        require(snapshot.has_value(), "snapshot expected");
        const auto result = control.step(*snapshot);
        require(result.has_value(), "Control result expected");
        const auto record = makeRecord(*snapshot, *result);
        require(record && isValidRecord(*record), "an emitted record must be valid");
        require(record->cycle == 7 && record->gatewayConfig.maxAgeCycles == 1 && record->unknownId &&
                    record->duplicates[4] && record->state == ControlState::safe_state &&
                    record->action == result->action && record->diagnosticCount == result->diagnostics.size() &&
                    record->latchedFault && record->latchedFault->cycle == 7,
                "the record must capture both ECU outputs");
        for (std::size_t index = 0; index < signalCount; ++index) {
            expectObservation(record->signals[index], *snapshot->observe(signalCatalog[index].id));
        }
        for (std::size_t index = 0; index < record->diagnosticCount; ++index) {
            require(sameDiagnostic(record->diagnostics[index], result->diagnostics.begin()[index]),
                    "the record must preserve emitted diagnostics");
        }
        const auto later = gateway.processCycle(9, SignalBatch{});
        require(later && !makeRecord(*later, *result), "mixed cycles must be rejected");
        return *record;
    }();
    require(saved.signals[2].sample && saved.signals[2].sample->value == 85.5 && saved.signals[2].age == 0,
            "the record must own its data beyond Gateway lifetime");
}

std::string writeRecords(const std::vector<CycleRecord>& records)
{
    std::ostringstream output;
    CsvWriter writer(output);
    for (const auto& record : records) {
        require(writer.write(record), "writing a valid record must succeed");
    }
    require(writer.finish(), "finishing the CSV must succeed");
    return output.str();
}

void expectRead(std::istream& input, const std::vector<CycleRecord>& records)
{
    CsvReader reader(input);
    CycleRecord record;
    for (const auto& expected : records) {
        require(reader.read(record) == CsvReadStatus::record, "a complete record must be readable");
        expectRecord(record, expected);
    }
    require(reader.read(record) == CsvReadStatus::end && reader.read(record) == CsvReadStatus::end &&
                reader.error() == CsvError::none && reader.line() == records.size() + 1,
            "clean EOF must stay distinct from a read error");
}

class CommaLocale : public std::numpunct<char> {
protected:
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};

void roundTrip()
{
    const auto maxCycle = std::numeric_limits<Cycle>::max();
    Gateway gateway{GatewayConfig{maxCycle}};
    ControlECU control;
    const auto initial = makeRecord(gateway.snapshot(), ControlResult{});
    require(initial.has_value(), "the initial record must represent all samples as absent");
    std::vector<CycleRecord> expected{*initial};
    SignalBatch batch;
    batch.add(SignalId::speed, -0.0);
    batch.add(SignalId::engine_rpm, std::numeric_limits<double>::denorm_min());
    batch.add(SignalId::coolant_temperature, std::numeric_limits<double>::quiet_NaN());
    batch.add(SignalId::throttle_position, std::numeric_limits<double>::infinity());
    batch.add(SignalId::battery_voltage, -std::numeric_limits<double>::infinity());
    batch.add(SignalId::oil_pressure, std::numeric_limits<double>::max());
    SignalBatch malformedBatch;
    malformedBatch.add(SignalId::speed, 1.0);
    malformedBatch.add(SignalId::speed, 2.0);
    malformedBatch.add(static_cast<SignalId>(255), 42.0);
    for (const auto cycle : std::array<Cycle, 2>{1, maxCycle}) {
        const auto snapshot = gateway.processCycle(cycle, cycle == 1 ? batch : malformedBatch);
        require(snapshot.has_value(), "numeric fixture must publish");
        const auto result = control.step(*snapshot, cycle == 1 ? ControlEvent::none : ControlEvent::shutdown);
        require(result.has_value(), "numeric fixture must reach Control");
        const auto record = makeRecord(*snapshot, *result);
        require(record.has_value(), "numeric fixture must be recordable");
        expected.push_back(*record);
    }

    const std::locale alternate(std::locale::classic(), new CommaLocale);
    std::ostringstream output;
    output.imbue(alternate);
    CsvWriter writer(output);
    for (const auto& record : expected) {
        require(writer.write(record), "edge values must be writable");
    }
    require(writer.finish() && writer.finish(), "finish must be idempotent");
    const auto csv = output.str();
    require(csv == writeRecords(expected) && csv.find(",nan,NON_FINITE,") != std::string::npos &&
                csv.find(",+inf,NON_FINITE,") != std::string::npos && csv.find(",-inf,NON_FINITE,") != std::string::npos,
            "the numeric text must be independent of locale and preserve non-finite categories");
    std::istringstream input(csv);
    input.imbue(alternate);
    expectRead(input, expected);

    std::string crlf;
    for (const auto ch : csv) {
        if (ch == '\n') {
            crlf.push_back('\r');
        }
        crlf.push_back(ch);
    }
    std::istringstream windows(crlf);
    expectRead(windows, expected);
    std::istringstream unterminated(csv.substr(0, csv.size() - 1));
    unterminated.exceptions(std::ios::eofbit | std::ios::failbit);
    expectRead(unterminated, expected);
    std::istringstream headerOnly(writeRecords({}));
    expectRead(headerOnly, {});
}

std::vector<std::string> columns(std::string_view line)
{
    std::vector<std::string> result;
    while (true) {
        const auto comma = line.find(',');
        result.emplace_back(line.substr(0, comma));
        if (comma == std::string_view::npos) {
            return result;
        }
        line.remove_prefix(comma + 1);
    }
}

std::string replaceField(std::string_view header, std::string_view row, std::string_view key, std::string_view value)
{
    const auto names = columns(header);
    auto fields = columns(row);
    const auto found = std::find(names.begin(), names.end(), key);
    require(found != names.end() && fields.size() == names.size(), "mutation needs a valid column");
    fields[static_cast<std::size_t>(found - names.begin())] = value;
    std::string result = fields.front();
    for (std::size_t index = 1; index < fields.size(); ++index) {
        result += ',';
        result += fields[index];
    }
    return result;
}

void malformed()
{
    const auto records = capture(ScenarioKind::faults);
    std::istringstream original(writeRecords({records[0], records[1]}));
    std::string header;
    std::string first;
    std::string second;
    require(static_cast<bool>(std::getline(original, header) && std::getline(original, first) &&
                              std::getline(original, second)), "the mutation fixture needs two records");
    require(columns(header).size() == 74, "CSV version 1 must have its documented columns");

    const auto check = [&](const std::string& row, CsvError expected) {
        std::istringstream input(header + '\n' + first + '\n' + row);
        CsvReader reader(input);
        CycleRecord decoded;
        require(reader.read(decoded) == CsvReadStatus::record, "the valid prefix must be readable");
        require(reader.read(decoded) == CsvReadStatus::error && reader.error() == expected && reader.line() == 3 &&
                    reader.read(decoded) == CsvReadStatus::error,
                "a malformed row must report its line and stop reading");
        expectRecord(decoded, records.front());
    };
    const auto mutate = [&](std::string_view field, std::string_view value, CsvError expected) {
        check(replaceField(header, second, field, value), expected);
    };
    mutate("version", "2", CsvError::unsupported_version);
    mutate("cycle", "18446744073709551616", CsvError::invalid_record);
    mutate("cycle", "-1", CsvError::invalid_record);
    mutate("control_state", "UNKNOWN", CsvError::invalid_record);
    mutate("control_action", "\"NORMAL_OPERATION\"", CsvError::invalid_record);
    mutate("unknown_id", "2", CsvError::invalid_record);
    mutate("speed_value", "1.2junk", CsvError::invalid_record);
    mutate("speed_value", "1e9999", CsvError::invalid_record);
    mutate("speed_value", "nan", CsvError::invalid_record);
    mutate("speed_value", "", CsvError::invalid_record);
    mutate("speed_age", "1", CsvError::invalid_record);
    mutate("speed_received_at", "3", CsvError::invalid_record);
    mutate("speed_status", "MISSING", CsvError::invalid_record);
    mutate("diagnostic_count", "8", CsvError::invalid_record);
    mutate("diag_0_code", "SIGNAL_MISSING", CsvError::invalid_record);
    mutate("latched_cycle", "2", CsvError::invalid_record);
    check(first, CsvError::cycle_order);
    check(second.substr(0, second.size() - 1), CsvError::invalid_record);
    check(second + ',', CsvError::invalid_record);
    check(std::string(4097, 'x'), CsvError::line_too_long);

    for (const auto badHeader : {"", "version,cycle\n"}) {
        std::istringstream input(badHeader);
        CsvReader reader(input);
        CycleRecord decoded;
        require(reader.read(decoded) == CsvReadStatus::error && reader.error() == CsvError::invalid_header &&
                    reader.line() == 1,
                "missing or malformed header must be rejected");
    }
}

void streamErrors()
{
    const auto records = capture(ScenarioKind::nominal);
    const auto prefix = writeRecords({records[0]});
    LimitedOutput buffer(prefix.size() + 9);
    std::ostream output(&buffer);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    CsvWriter writer(output);
    require(writer.write(records[0]) && !writer.write(records[1]) && writer.error() == CsvError::io_error &&
                !writer.write(records[2]) && !writer.finish(),
            "a short write must make the writer fail permanently");
    std::istringstream partial(buffer.bytes);
    CsvReader reader(partial);
    CycleRecord decoded;
    require(reader.read(decoded) == CsvReadStatus::record, "the completed prefix must survive the failed write");
    expectRecord(decoded, records[0]);
    require(reader.read(decoded) == CsvReadStatus::error && reader.error() == CsvError::invalid_record,
            "the partial row must be rejected");

    LimitedOutput flushBuffer(8192);
    std::ostream flushOutput(&flushBuffer);
    CsvWriter flushWriter(flushOutput);
    require(flushWriter.write(records[0]), "the record must reach the stream before the flush failure");
    flushBuffer.failFlush = true;
    require(!flushWriter.finish() && flushWriter.error() == CsvError::io_error, "flush errors must be reported");

    std::ostringstream invalidOutput;
    CsvWriter invalidWriter(invalidOutput);
    auto invalid = records[0];
    invalid.diagnosticCount = Diagnostics::capacity + 1;
    require(!invalidWriter.write(invalid) && invalidWriter.error() == CsvError::invalid_record &&
                invalidOutput.str().empty(), "invalid records must be rejected before writing any bytes");
    std::ostringstream orderedOutput;
    CsvWriter ordered(orderedOutput);
    require(ordered.write(records[0]) && !ordered.write(records[0]) && ordered.error() == CsvError::cycle_order &&
                orderedOutput.str() == prefix, "a repeated cycle must not add bytes");
    std::ostringstream finishedOutput;
    CsvWriter finished(finishedOutput);
    require(finished.finish() && !finished.write(records[0]) && finished.error() == CsvError::closed,
            "finished writers must reject further records");

    std::istringstream broken(prefix);
    broken.setstate(std::ios::badbit);
    CsvReader brokenReader(broken);
    require(brokenReader.read(decoded) == CsvReadStatus::error && brokenReader.error() == CsvError::io_error,
            "I/O errors must stay distinct from malformed data or EOF");
}

void observerFailure()
{
    const auto scenario = makeScenario(ScenarioKind::faults);
    const auto expected = capture(ScenarioKind::faults);
    require(scenario.has_value(), "fault scenario expected");
    for (const std::size_t accepted : {0u, 3u, 41u}) {
        MemorySink sink{accepted, 0, {}};
        const auto record = recordObserver(sink);
        std::size_t visits = 0;
        const bool completed = runScenario(*scenario, [&](const SignalSnapshot& snapshot, const ControlResult& result) {
            require(visits < expected.size() && result.state == expected[visits].state &&
                        result.cycle == expected[visits].cycle && result.action == expected[visits].action,
                    "a failing destination must see the original Control result");
            ++visits;
            return record(snapshot, result);
        });
        require(!completed && sink.calls == accepted + 1 && visits == accepted + 1 && sink.records.size() == accepted,
                "observer failure must stop at the failing cycle");
        for (std::size_t index = 0; index < accepted; ++index) {
            expectRecord(sink.records[index], expected[index]);
        }
    }
}

void scenarioRoundTrip()
{
    for (const auto kind : {ScenarioKind::nominal, ScenarioKind::faults, ScenarioKind::battery_fault,
                           ScenarioKind::oil_fault, ScenarioKind::temperature_dropout}) {
        const auto scenario = makeScenario(kind);
        require(scenario.has_value(), "scenario expected");
        std::ostringstream output;
        CsvWriter writer(output);
        require(runScenario(*scenario, recordObserver(writer)) && writer.finish(), "the observer must log the full scenario");
        std::istringstream input(output.str());
        expectRead(input, capture(kind));
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
        if (argc != 2) {
            throw std::runtime_error("expected a CSV test name");
        }
        struct TestCase { std::string_view name; void (*run)(); };
        const std::array cases{
            TestCase{"record", recordCapture}, TestCase{"round_trip", roundTrip},
            TestCase{"malformed", malformed}, TestCase{"stream_errors", streamErrors},
            TestCase{"observer_failure", observerFailure}, TestCase{"scenarios", scenarioRoundTrip},
        };
        for (const auto& test : cases) {
            if (test.name == argv[1]) {
                test.run();
                return EXIT_SUCCESS;
            }
        }
        throw std::runtime_error("unknown CSV test name");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
