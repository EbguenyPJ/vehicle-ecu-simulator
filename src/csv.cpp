#include "ecu/csv.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <istream>
#include <limits>
#include <ostream>
#include <string>

namespace ecu {
namespace {

constexpr std::string_view versionToken = "1";
constexpr std::size_t maxLineSize = 4096;
constexpr std::array signalColumns{"value", "quality", "received_at", "age", "freshness", "status", "duplicate"};
constexpr std::size_t fieldCount = 7 + signalCount * signalColumns.size() + Diagnostics::capacity * 3 + 4;

template <typename Enum>
struct Name {
    constexpr Name(Enum enumValue, std::string_view text) noexcept : value(enumValue), token(text) {}

    Enum value;
    std::string_view token;
};

constexpr std::array signalNames{
    Name{SignalId::speed, "speed"},
    Name{SignalId::engine_rpm, "engine_rpm"},
    Name{SignalId::coolant_temperature, "coolant_temperature"},
    Name{SignalId::throttle_position, "throttle_position"},
    Name{SignalId::battery_voltage, "battery_voltage"},
    Name{SignalId::oil_pressure, "oil_pressure"},
};

constexpr std::array stateNames{
    Name{ControlState::init, "INIT"}, Name{ControlState::self_test, "SELF_TEST"},
    Name{ControlState::operational, "OPERATIONAL"}, Name{ControlState::degraded, "DEGRADED"},
    Name{ControlState::safe_state, "SAFE_STATE"}, Name{ControlState::shutdown, "SHUTDOWN"},
};

constexpr std::array actionNames{
    Name{ControlAction::inhibit_output, "INHIBIT_OUTPUT"},
    Name{ControlAction::limited_operation, "LIMITED_OPERATION"},
    Name{ControlAction::normal_operation, "NORMAL_OPERATION"},
};

constexpr std::array qualityNames{
    Name{SampleQuality::in_range, "IN_RANGE"}, Name{SampleQuality::out_of_range, "OUT_OF_RANGE"},
    Name{SampleQuality::non_finite, "NON_FINITE"},
};

constexpr std::array freshnessNames{
    Name{Freshness::never_received, "NEVER_RECEIVED"}, Name{Freshness::fresh, "FRESH"},
    Name{Freshness::retained, "RETAINED"}, Name{Freshness::expired, "EXPIRED"},
};

constexpr std::array statusNames{
    Name{SignalStatus::valid, "VALID"}, Name{SignalStatus::invalid, "INVALID"},
    Name{SignalStatus::missing, "MISSING"},
};

constexpr std::array severityNames{
    Name{Severity::degraded, "DEGRADED"}, Name{Severity::critical, "CRITICAL"},
};

constexpr std::array diagnosticNames{
    Name{DiagnosticCode::signal_missing, "SIGNAL_MISSING"},
    Name{DiagnosticCode::signal_expired, "SIGNAL_EXPIRED"},
    Name{DiagnosticCode::signal_retained, "SIGNAL_RETAINED"},
    Name{DiagnosticCode::signal_out_of_range, "SIGNAL_OUT_OF_RANGE"},
    Name{DiagnosticCode::signal_non_finite, "SIGNAL_NON_FINITE"},
    Name{DiagnosticCode::coolant_over_temperature, "COOLANT_OVER_TEMPERATURE"},
    Name{DiagnosticCode::low_oil_pressure, "LOW_OIL_PRESSURE"},
    Name{DiagnosticCode::battery_voltage_outside_operating_range, "BATTERY_VOLTAGE_OUTSIDE_OPERATING_RANGE"},
    Name{DiagnosticCode::powertrain_incoherence, "POWERTRAIN_INCOHERENCE"},
    Name{DiagnosticCode::recovery_pending, "RECOVERY_PENDING"},
};

template <typename Enum, std::size_t Count>
std::string_view tokenOf(Enum value, const std::array<Name<Enum>, Count>& names)
{
    const auto entry = std::find_if(names.begin(), names.end(), [value](const auto& name) {
        return name.value == value;
    });
    return entry == names.end() ? std::string_view{} : entry->token;
}

template <typename Enum, std::size_t Count>
bool parseEnum(std::string_view token, Enum& value, const std::array<Name<Enum>, Count>& names)
{
    const auto entry = std::find_if(names.begin(), names.end(), [token](const auto& name) {
        return name.token == token;
    });
    if (entry == names.end()) {
        return false;
    }
    value = entry->value;
    return true;
}

void appendField(std::string& row, std::string_view field = {})
{
    row.push_back(',');
    row.append(field);
}

void appendInteger(std::string& row, Cycle value)
{
    std::array<char, std::numeric_limits<Cycle>::digits10 + 1> buffer{};
    const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    appendField(row, {buffer.data(), static_cast<std::size_t>(written.ptr - buffer.data())});
}

void appendValue(std::string& row, double value)
{
    if (std::isnan(value)) {
        appendField(row, "nan");
    } else if (std::isinf(value)) {
        appendField(row, std::signbit(value) ? "-inf" : "+inf");
    } else {
        // Espacio para signo, punto y exponente además de los dígitos significativos.
        std::array<char, std::numeric_limits<double>::max_digits10 + 16> buffer{};
        const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                           std::chars_format::general, std::numeric_limits<double>::max_digits10);
        appendField(row, {buffer.data(), static_cast<std::size_t>(written.ptr - buffer.data())});
    }
}

const std::string& csvHeader()
{
    static const auto header = [] {
        std::string line{"version,cycle,max_age_cycles,control_state,control_action,unknown_id,diagnostic_count"};
        for (const auto& descriptor : signalCatalog) {
            const auto key = tokenOf(descriptor.id, signalNames);
            for (const auto column : signalColumns) {
                appendField(line, key);
                line.push_back('_');
                line.append(column);
            }
        }
        for (std::size_t index = 0; index < Diagnostics::capacity; ++index) {
            const auto prefix = "diag_" + std::to_string(index);
            appendField(line, prefix + "_code");
            appendField(line, prefix + "_severity");
            appendField(line, prefix + "_signal");
        }
        line.append(",latched_cycle,latched_code,latched_severity,latched_signal");
        return line;
    }();
    return header;
}

void appendDiagnostic(std::string& row, const Diagnostic* diagnostic)
{
    appendField(row, diagnostic ? tokenOf(diagnostic->code, diagnosticNames) : std::string_view{});
    appendField(row, diagnostic ? tokenOf(diagnostic->severity, severityNames) : std::string_view{});
    appendField(row, diagnostic && diagnostic->signal ? tokenOf(*diagnostic->signal, signalNames) : std::string_view{});
}

std::string encodeRow(const CycleRecord& record)
{
    std::string row{versionToken};
    appendInteger(row, record.cycle);
    appendInteger(row, record.gatewayConfig.maxAgeCycles);
    appendField(row, tokenOf(record.state, stateNames));
    appendField(row, tokenOf(record.action, actionNames));
    appendField(row, record.unknownId ? "1" : "0");
    appendInteger(row, static_cast<Cycle>(record.diagnosticCount));
    for (std::size_t index = 0; index < signalCount; ++index) {
        const auto& signal = record.signals[index];
        if (signal.sample) {
            appendValue(row, signal.sample->value);
            appendField(row, tokenOf(signal.sample->quality, qualityNames));
            appendInteger(row, signal.sample->receivedAt);
            appendInteger(row, *signal.age);
        } else {
            row.append(",,,,");
        }
        appendField(row, tokenOf(signal.freshness, freshnessNames));
        appendField(row, tokenOf(signal.status, statusNames));
        appendField(row, record.duplicates[index] ? "1" : "0");
    }
    for (std::size_t index = 0; index < record.diagnostics.size(); ++index) {
        appendDiagnostic(row, index < record.diagnosticCount ? &record.diagnostics[index] : nullptr);
    }
    if (record.latchedFault) {
        appendInteger(row, record.latchedFault->cycle);
        appendDiagnostic(row, &record.latchedFault->cause);
    } else {
        row.append(",,,,");
    }
    return row;
}

bool parseInteger(std::string_view field, Cycle& value)
{
    if (field.empty()) {
        return false;
    }
    const auto end = field.data() + field.size();
    const auto parsed = std::from_chars(field.data(), end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parseValue(std::string_view field, double& value)
{
    if (field == "nan") {
        value = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    if (field == "+inf" || field == "-inf") {
        value = field.front() == '-' ? -std::numeric_limits<double>::infinity()
                                     : std::numeric_limits<double>::infinity();
        return true;
    }
    if (field.empty()) {
        return false;
    }
    const auto end = field.data() + field.size();
    const auto parsed = std::from_chars(field.data(), end, value, std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == end && std::isfinite(value);
}

bool parseFlag(std::string_view field, bool& value)
{
    if (field != "0" && field != "1") {
        return false;
    }
    value = field == "1";
    return true;
}

using Fields = std::array<std::string_view, fieldCount>;

bool splitFields(std::string_view line, Fields& fields)
{
    for (auto& field : fields) {
        const auto comma = line.find(',');
        field = line.substr(0, comma);
        if (comma == std::string_view::npos) {
            return &field == &fields.back();
        }
        line.remove_prefix(comma + 1);
    }
    return false;
}

struct RowFields {
    const Fields& fields;
    std::size_t next{0};

    std::string_view take() noexcept { return fields[next++]; }
};

bool parseObservation(RowFields& row, SignalObservation& observation, bool& duplicate)
{
    const auto value = row.take();
    const auto quality = row.take();
    const auto receivedAt = row.take();
    const auto age = row.take();
    if (value.empty()) {
        if (!quality.empty() || !receivedAt.empty() || !age.empty()) {
            return false;
        }
    } else {
        SignalSample sample;
        Cycle sampleAge = 0;
        if (!parseValue(value, sample.value) || !parseEnum(quality, sample.quality, qualityNames) ||
            !parseInteger(receivedAt, sample.receivedAt) || !parseInteger(age, sampleAge)) {
            return false;
        }
        observation.sample = sample;
        observation.age = sampleAge;
    }
    return parseEnum(row.take(), observation.freshness, freshnessNames) &&
           parseEnum(row.take(), observation.status, statusNames) && parseFlag(row.take(), duplicate);
}

bool parseDiagnostic(RowFields& row, Diagnostic* diagnostic)
{
    const auto code = row.take();
    const auto severity = row.take();
    const auto signal = row.take();
    if (!diagnostic) {
        return code.empty() && severity.empty() && signal.empty();
    }
    if (!parseEnum(code, diagnostic->code, diagnosticNames) ||
        !parseEnum(severity, diagnostic->severity, severityNames)) {
        return false;
    }
    if (!signal.empty()) {
        SignalId id{};
        if (!parseEnum(signal, id, signalNames)) {
            return false;
        }
        diagnostic->signal = id;
    }
    return true;
}

CsvError decodeRow(std::string_view line, CycleRecord& record)
{
    Fields fields{};
    if (!splitFields(line, fields)) {
        return CsvError::invalid_record;
    }
    RowFields row{fields};
    const auto version = row.take();
    if (version != versionToken) {
        Cycle parsedVersion = 0;
        return parseInteger(version, parsedVersion) && parsedVersion != 1
            ? CsvError::unsupported_version : CsvError::invalid_record;
    }
    Cycle count = 0;
    if (!parseInteger(row.take(), record.cycle) || !parseInteger(row.take(), record.gatewayConfig.maxAgeCycles) ||
        !parseEnum(row.take(), record.state, stateNames) || !parseEnum(row.take(), record.action, actionNames) ||
        !parseFlag(row.take(), record.unknownId) || !parseInteger(row.take(), count) || count > Diagnostics::capacity) {
        return CsvError::invalid_record;
    }
    record.diagnosticCount = static_cast<std::size_t>(count);
    for (std::size_t index = 0; index < signalCount; ++index) {
        if (!parseObservation(row, record.signals[index], record.duplicates[index])) {
            return CsvError::invalid_record;
        }
    }
    for (std::size_t index = 0; index < record.diagnostics.size(); ++index) {
        if (!parseDiagnostic(row, index < record.diagnosticCount ? &record.diagnostics[index] : nullptr)) {
            return CsvError::invalid_record;
        }
    }
    const auto latchedCycle = row.take();
    if (latchedCycle.empty()) {
        if (!parseDiagnostic(row, nullptr)) {
            return CsvError::invalid_record;
        }
    } else {
        LatchedFault fault;
        if (!parseInteger(latchedCycle, fault.cycle) || !parseDiagnostic(row, &fault.cause)) {
            return CsvError::invalid_record;
        }
        record.latchedFault = fault;
    }
    return isValidRecord(record) ? CsvError::none : CsvError::invalid_record;
}

using LineBuffer = std::array<char, maxLineSize + 2>;
enum class LineStatus { line, end, io_error, too_long };

LineStatus readLine(std::istream& input, LineBuffer& buffer, std::string_view& line)
{
    if (input.bad() || (input.fail() && !input.eof())) {
        return LineStatus::io_error;
    }
    try {
        input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    } catch (const std::ios_base::failure&) {
        // Los flujos con exceptions() también pueden señalar EOF por excepción.
    } catch (...) {
        return LineStatus::io_error;
    }
    const auto count = static_cast<std::size_t>(input.gcount());
    if (input.bad()) {
        return LineStatus::io_error;
    }
    if (input.eof() && count == 0) {
        return LineStatus::end;
    }
    if (input.fail() || count == 0) {
        return count == buffer.size() - 1 ? LineStatus::too_long : LineStatus::io_error;
    }
    line = {buffer.data(), input.eof() ? count : count - 1};
    if (!input.eof() && !line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line.size() > maxLineSize ? LineStatus::too_long : LineStatus::line;
}

CsvError lineError(LineStatus status, CsvError fallback) noexcept
{
    if (status == LineStatus::too_long) {
        return CsvError::line_too_long;
    }
    return status == LineStatus::io_error ? CsvError::io_error : fallback;
}

}  // namespace

std::string_view csvErrorMessage(CsvError error) noexcept
{
    switch (error) {
    case CsvError::none: return "sin error";
    case CsvError::io_error: return "error de entrada o salida";
    case CsvError::invalid_header: return "cabecera CSV invalida";
    case CsvError::unsupported_version: return "version CSV no compatible";
    case CsvError::invalid_record: return "registro CSV invalido";
    case CsvError::cycle_order: return "ciclos fuera de orden";
    case CsvError::line_too_long: return "linea CSV demasiado larga";
    case CsvError::closed: return "escritor CSV finalizado";
    }
    return "error CSV desconocido";
}

bool CsvWriter::writeLine(std::string_view line)
{
    if (line.size() > maxLineSize) {
        error_ = CsvError::line_too_long;
        return false;
    }
    try {
        output_.write(line.data(), static_cast<std::streamsize>(line.size()));
        output_.put('\n');
    } catch (...) {
        error_ = CsvError::io_error;
        return false;
    }
    if (!output_) {
        error_ = CsvError::io_error;
        return false;
    }
    return true;
}

bool CsvWriter::writeHeader()
{
    if (!headerWritten_) {
        headerWritten_ = writeLine(csvHeader());
    }
    return headerWritten_;
}

bool CsvWriter::write(const CycleRecord& record)
{
    if (error_ != CsvError::none) {
        return false;
    }
    if (finished_) {
        error_ = CsvError::closed;
        return false;
    }
    if (!isValidRecord(record)) {
        error_ = CsvError::invalid_record;
        return false;
    }
    if (lastCycle_ && record.cycle <= *lastCycle_) {
        error_ = CsvError::cycle_order;
        return false;
    }
    if (!writeHeader() || !writeLine(encodeRow(record))) {
        return false;
    }
    lastCycle_ = record.cycle;
    return true;
}

bool CsvWriter::finish()
{
    if (error_ != CsvError::none) {
        return false;
    }
    if (finished_) {
        return true;
    }
    if (!writeHeader()) {
        return false;
    }
    try {
        output_.flush();
    } catch (...) {
        error_ = CsvError::io_error;
        return false;
    }
    if (!output_) {
        error_ = CsvError::io_error;
        return false;
    }
    finished_ = true;
    return true;
}

CsvReadStatus CsvReader::read(CycleRecord& record)
{
    if (error_ != CsvError::none) {
        return CsvReadStatus::error;
    }
    if (finished_) {
        return CsvReadStatus::end;
    }
    LineBuffer buffer{};
    std::string_view line;
    if (!headerRead_) {
        line_ = 1;
        const auto status = readLine(input_, buffer, line);
        if (status != LineStatus::line || line != csvHeader()) {
            error_ = lineError(status, CsvError::invalid_header);
            return CsvReadStatus::error;
        }
        headerRead_ = true;
    }
    const auto status = readLine(input_, buffer, line);
    if (status == LineStatus::end) {
        finished_ = true;
        return CsvReadStatus::end;
    }
    ++line_;
    if (status != LineStatus::line) {
        error_ = lineError(status, CsvError::invalid_record);
        return CsvReadStatus::error;
    }

    CycleRecord decoded;
    error_ = decodeRow(line, decoded);
    if (error_ != CsvError::none) {
        return CsvReadStatus::error;
    }
    if (lastCycle_ && decoded.cycle <= *lastCycle_) {
        error_ = CsvError::cycle_order;
        return CsvReadStatus::error;
    }
    record = decoded;
    lastCycle_ = decoded.cycle;
    return CsvReadStatus::record;
}

}  // namespace ecu
