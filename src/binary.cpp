#include "ecu/binary.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <istream>
#include <limits>
#include <ostream>
#include <type_traits>

#include "ecu/scaling.hpp"

namespace ecu {
namespace {

static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::digits == 53 && std::numeric_limits<double>::max_exponent == 1024,
              "The binary format uses IEEE 754 binary64");
static_assert(signalCount == 6 && Diagnostics::capacity == 7, "Binary version 1 fixes the catalog and its capacity");
static_assert(coolantTemperatureScale.factor == 0.1 && coolantTemperatureScale.offset == -40.0 &&
              coolantTemperatureScale.rawBits == 16 && batteryVoltageScale.factor == 0.05 &&
              batteryVoltageScale.offset == 8.0 && batteryVoltageScale.rawBits == 8,
              "Binary version 1 fixes the scaling profiles");

constexpr std::array<std::uint8_t, 8> header{0x45, 0x43, 0x55, 0x42, 1, 0, 0, 0};
constexpr std::size_t lengthSize = 2;
constexpr std::size_t crcSize = 4;
constexpr std::size_t minPayloadSize = 20 + signalCount * 3;
constexpr std::size_t maxPayloadSize = 20 + signalCount * 20 + Diagnostics::capacity * 3 + 11;
constexpr std::uint8_t latchBit = 0x80;
constexpr std::uint8_t noSignal = 0xff;
using Frame = std::array<std::uint8_t, lengthSize + maxPayloadSize + crcSize>;

constexpr std::array signals{
    SignalId::speed, SignalId::engine_rpm, SignalId::coolant_temperature,
    SignalId::throttle_position, SignalId::battery_voltage, SignalId::oil_pressure,
};
constexpr bool catalogMatches() noexcept
{
    for (std::size_t index = 0; index < signals.size(); ++index) {
        if (signals[index] != signalCatalog[index].id) {
            return false;
        }
    }
    return true;
}
static_assert(catalogMatches(), "Binary version 1 fixes signal order");

// La posición en estas tablas es el código del archivo.
constexpr std::array states{
    ControlState::init, ControlState::self_test, ControlState::operational,
    ControlState::degraded, ControlState::safe_state, ControlState::shutdown,
};
constexpr std::array actions{
    ControlAction::inhibit_output, ControlAction::limited_operation, ControlAction::normal_operation,
};
constexpr std::array qualities{SampleQuality::in_range, SampleQuality::out_of_range, SampleQuality::non_finite};
constexpr std::array freshnesses{Freshness::never_received, Freshness::fresh, Freshness::retained, Freshness::expired};
constexpr std::array statuses{SignalStatus::valid, SignalStatus::invalid, SignalStatus::missing};
constexpr std::array severities{Severity::degraded, Severity::critical};
constexpr std::array diagnostics{
    DiagnosticCode::signal_missing, DiagnosticCode::signal_expired, DiagnosticCode::signal_retained,
    DiagnosticCode::signal_out_of_range, DiagnosticCode::signal_non_finite, DiagnosticCode::coolant_over_temperature,
    DiagnosticCode::low_oil_pressure, DiagnosticCode::battery_voltage_outside_operating_range,
    DiagnosticCode::powertrain_incoherence, DiagnosticCode::recovery_pending,
};

enum class ValueKind : std::uint8_t { absent = 0, binary64 = 1, scaled = 2 };

std::uint64_t doubleBits(double value) noexcept
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double fromBits(std::uint64_t bits) noexcept
{
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint32_t crc32(const std::uint8_t* bytes, std::size_t size) noexcept
{
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1u) ^ ((crc & 1u) != 0 ? 0xedb88320u : 0u);
        }
    }
    return crc ^ 0xffffffffu;
}

class ByteWriter {
public:
    explicit ByteWriter(Frame& frame) noexcept : frame_(frame) {}

    template <typename UInt>
    void integer(UInt value) noexcept
    {
        static_assert(std::is_unsigned_v<UInt> && sizeof(UInt) <= sizeof(std::uint64_t));
        if (sizeof(UInt) > frame_.size() - position_) {
            good_ = false;
            return;
        }
        const auto bits = static_cast<std::uint64_t>(value);
        for (std::size_t index = 0; index < sizeof(UInt); ++index) {
            frame_[position_++] = static_cast<std::uint8_t>(bits >> (index * 8u));
        }
    }

    template <typename Enum, std::size_t Count>
    void enumeration(Enum value, const std::array<Enum, Count>& codes) noexcept
    {
        const auto found = std::find(codes.begin(), codes.end(), value);
        if (found == codes.end()) {
            good_ = false;
            return;
        }
        integer(static_cast<std::uint8_t>(found - codes.begin()));
    }

    [[nodiscard]] bool good() const noexcept { return good_; }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }

private:
    Frame& frame_;
    std::size_t position_{lengthSize};
    bool good_{true};
};

class ByteReader {
public:
    ByteReader(const std::uint8_t* bytes, std::size_t size) noexcept : bytes_(bytes), size_(size) {}

    template <typename UInt>
    bool integer(UInt& value) noexcept
    {
        static_assert(std::is_unsigned_v<UInt> && sizeof(UInt) <= sizeof(std::uint64_t));
        if (sizeof(UInt) > size_ - position_) {
            return false;
        }
        std::uint64_t bits = 0;
        for (std::size_t index = 0; index < sizeof(UInt); ++index) {
            bits |= static_cast<std::uint64_t>(bytes_[position_++]) << (index * 8u);
        }
        value = static_cast<UInt>(bits);
        return true;
    }

    template <typename Enum, std::size_t Count>
    bool enumeration(Enum& value, const std::array<Enum, Count>& codes) noexcept
    {
        std::uint8_t code = 0;
        if (!integer(code) || code >= codes.size()) {
            return false;
        }
        value = codes[code];
        return true;
    }

    [[nodiscard]] bool empty() const noexcept { return position_ == size_; }

private:
    const std::uint8_t* bytes_;
    std::size_t size_;
    std::size_t position_{0};
};

std::optional<std::uint16_t> exactScaled(double value, const LinearScale& scale) noexcept
{
    const auto encoded = encodePhysical(value, scale);
    if (!encoded || encoded->saturation != Saturation::none) {
        return std::nullopt;
    }
    const auto physical = decodeRaw(encoded->raw, scale);
    if (!physical || doubleBits(*physical) != doubleBits(value)) {
        return std::nullopt;
    }
    return encoded->raw;
}

void encodeObservation(ByteWriter& writer, SignalId id, const SignalObservation& observation) noexcept
{
    writer.enumeration(observation.status, statuses);
    writer.enumeration(observation.freshness, freshnesses);
    if (!observation.sample) {
        writer.integer(static_cast<std::uint8_t>(ValueKind::absent));
        return;
    }

    const auto& sample = *observation.sample;
    const auto scale = std::isfinite(sample.value) ? scalingFor(id) : std::nullopt;
    const auto raw = scale ? exactScaled(sample.value, *scale) : std::nullopt;
    writer.integer(static_cast<std::uint8_t>(raw ? ValueKind::scaled : ValueKind::binary64));
    writer.enumeration(sample.quality, qualities);
    writer.integer(sample.receivedAt);
    if (!raw) {
        writer.integer(doubleBits(sample.value));
    } else if (scale->rawBits == 8) {
        writer.integer(static_cast<std::uint8_t>(*raw));
    } else {
        writer.integer(*raw);
    }
}

bool decodeScaled(ByteReader& reader, SignalId id, double& value) noexcept
{
    const auto scale = scalingFor(id);
    if (!scale) {
        return false;
    }
    std::uint16_t raw = 0;
    if (scale->rawBits == 8) {
        std::uint8_t byte = 0;
        if (!reader.integer(byte)) {
            return false;
        }
        raw = byte;
    } else if (!reader.integer(raw)) {
        return false;
    }
    const auto decoded = decodeRaw(raw, *scale);
    if (!decoded) {
        return false;
    }
    value = *decoded;
    return true;
}

bool decodeObservation(ByteReader& reader, SignalId id, Cycle cycle, SignalObservation& observation) noexcept
{
    std::uint8_t kind = 0;
    if (!reader.enumeration(observation.status, statuses) ||
        !reader.enumeration(observation.freshness, freshnesses) || !reader.integer(kind)) {
        return false;
    }
    if (kind == static_cast<std::uint8_t>(ValueKind::absent)) {
        return true;
    }

    SignalSample sample;
    if (!reader.enumeration(sample.quality, qualities) || !reader.integer(sample.receivedAt) ||
        sample.receivedAt > cycle) {
        return false;
    }
    switch (static_cast<ValueKind>(kind)) {
    case ValueKind::binary64: {
        std::uint64_t bits = 0;
        if (!reader.integer(bits)) {
            return false;
        }
        sample.value = fromBits(bits);
        break;
    }
    case ValueKind::scaled:
        if (!decodeScaled(reader, id, sample.value)) {
            return false;
        }
        break;
    default:
        return false;
    }
    observation.sample = sample;
    observation.age = cycle - sample.receivedAt;
    return true;
}

void encodeDiagnostic(ByteWriter& writer, const Diagnostic& diagnostic) noexcept
{
    writer.enumeration(diagnostic.code, diagnostics);
    writer.enumeration(diagnostic.severity, severities);
    if (diagnostic.signal) {
        writer.enumeration(*diagnostic.signal, signals);
    } else {
        writer.integer(noSignal);
    }
}

bool decodeDiagnostic(ByteReader& reader, Diagnostic& diagnostic) noexcept
{
    std::uint8_t code = 0;
    if (!reader.enumeration(diagnostic.code, diagnostics) || !reader.enumeration(diagnostic.severity, severities) ||
        !reader.integer(code)) {
        return false;
    }
    if (code == noSignal) {
        return true;
    }
    if (code >= signals.size()) {
        return false;
    }
    diagnostic.signal = signals[code];
    return true;
}

std::optional<std::size_t> encodeFrame(const CycleRecord& record, Frame& frame) noexcept
{
    ByteWriter writer(frame);
    writer.integer(record.cycle);
    writer.integer(record.gatewayConfig.maxAgeCycles);
    writer.enumeration(record.state, states);
    writer.enumeration(record.action, actions);

    unsigned flags = record.unknownId ? 1u : 0u;
    for (std::size_t index = 0; index < signals.size(); ++index) {
        if (record.duplicates[index]) {
            flags |= 1u << (index + 1);
        }
    }
    if (record.latchedFault) {
        flags |= latchBit;
    }
    writer.integer(static_cast<std::uint8_t>(flags));
    writer.integer(static_cast<std::uint8_t>(record.diagnosticCount));

    for (std::size_t index = 0; index < signals.size(); ++index) {
        encodeObservation(writer, signals[index], record.signals[index]);
    }
    for (std::size_t index = 0; index < record.diagnosticCount; ++index) {
        encodeDiagnostic(writer, record.diagnostics[index]);
    }
    if (record.latchedFault) {
        writer.integer(record.latchedFault->cycle);
        encodeDiagnostic(writer, record.latchedFault->cause);
    }
    const auto payloadSize = writer.position() - lengthSize;
    if (!writer.good() || payloadSize < minPayloadSize || payloadSize > maxPayloadSize) {
        return std::nullopt;
    }
    frame[0] = static_cast<std::uint8_t>(payloadSize);
    frame[1] = static_cast<std::uint8_t>(payloadSize >> 8u);
    writer.integer(crc32(frame.data(), writer.position()));
    return writer.good() ? std::optional<std::size_t>{writer.position()} : std::nullopt;
}

bool decodePayload(const std::uint8_t* bytes, std::size_t size, CycleRecord& record) noexcept
{
    ByteReader reader(bytes, size);
    std::uint8_t flags = 0;
    std::uint8_t count = 0;
    if (!reader.integer(record.cycle) || !reader.integer(record.gatewayConfig.maxAgeCycles) ||
        !reader.enumeration(record.state, states) || !reader.enumeration(record.action, actions) ||
        !reader.integer(flags) || !reader.integer(count) || count > record.diagnostics.size()) {
        return false;
    }
    record.unknownId = (flags & 1u) != 0;
    record.diagnosticCount = count;
    for (std::size_t index = 0; index < signals.size(); ++index) {
        record.duplicates[index] = (flags & (1u << (index + 1))) != 0;
        if (!decodeObservation(reader, signals[index], record.cycle, record.signals[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!decodeDiagnostic(reader, record.diagnostics[index])) {
            return false;
        }
    }
    if ((flags & latchBit) != 0) {
        LatchedFault fault;
        if (!reader.integer(fault.cycle) || !decodeDiagnostic(reader, fault.cause)) {
            return false;
        }
        record.latchedFault = fault;
    }
    return reader.empty() && isValidRecord(record);
}

enum class ChunkStatus { complete, end, truncated, io_error };

ChunkStatus readBytes(std::istream& input, std::uint8_t* bytes, std::size_t size)
{
    try {
        input.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(size));
    } catch (const std::ios_base::failure&) {
        // El estado del flujo distingue un EOF de otros fallos de lectura.
    } catch (...) {
        return ChunkStatus::io_error;
    }
    if (input.bad()) {
        return ChunkStatus::io_error;
    }
    if (input && input.gcount() == static_cast<std::streamsize>(size)) {
        return ChunkStatus::complete;
    }
    if (input.eof()) {
        return input.gcount() == 0 ? ChunkStatus::end : ChunkStatus::truncated;
    }
    return ChunkStatus::io_error;
}

BinaryError chunkError(ChunkStatus status) noexcept
{
    return status == ChunkStatus::io_error ? BinaryError::io_error : BinaryError::truncated;
}

}  // namespace

std::string_view binaryErrorMessage(BinaryError error) noexcept
{
    switch (error) {
    case BinaryError::none: return "sin error";
    case BinaryError::io_error: return "error de entrada o salida";
    case BinaryError::invalid_header: return "cabecera binaria invalida";
    case BinaryError::unsupported_version: return "version binaria no compatible";
    case BinaryError::invalid_length: return "longitud binaria invalida";
    case BinaryError::truncated: return "archivo binario truncado";
    case BinaryError::checksum_mismatch: return "CRC-32 incorrecto";
    case BinaryError::invalid_record: return "registro binario invalido";
    case BinaryError::cycle_order: return "ciclos fuera de orden";
    case BinaryError::closed: return "escritor binario finalizado";
    }
    return "error binario desconocido";
}

bool BinaryWriter::writeBytes(const std::uint8_t* bytes, std::size_t size)
{
    try {
        output_.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    } catch (...) {
        error_ = BinaryError::io_error;
        return false;
    }
    if (!output_) {
        error_ = BinaryError::io_error;
        return false;
    }
    return true;
}

bool BinaryWriter::writeHeader()
{
    if (!headerWritten_) {
        headerWritten_ = writeBytes(header.data(), header.size());
    }
    return headerWritten_;
}

bool BinaryWriter::write(const CycleRecord& record)
{
    if (error_ != BinaryError::none) {
        return false;
    }
    if (finished_) {
        error_ = BinaryError::closed;
        return false;
    }
    if (!isValidRecord(record)) {
        error_ = BinaryError::invalid_record;
        return false;
    }
    if (lastCycle_ && record.cycle <= *lastCycle_) {
        error_ = BinaryError::cycle_order;
        return false;
    }
    Frame frame{};
    const auto size = encodeFrame(record, frame);
    if (!size) {
        error_ = BinaryError::invalid_record;
        return false;
    }
    if (!writeHeader() || !writeBytes(frame.data(), *size)) {
        return false;
    }
    lastCycle_ = record.cycle;
    return true;
}

bool BinaryWriter::finish()
{
    if (error_ != BinaryError::none) {
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
        error_ = BinaryError::io_error;
        return false;
    }
    if (!output_) {
        error_ = BinaryError::io_error;
        return false;
    }
    finished_ = true;
    return true;
}

bool BinaryReader::readHeader()
{
    std::array<std::uint8_t, header.size()> received{};
    const auto status = readBytes(input_, received.data(), received.size());
    if (status != ChunkStatus::complete) {
        error_ = status == ChunkStatus::end ? BinaryError::invalid_header : chunkError(status);
        return false;
    }
    if (!std::equal(header.begin(), header.begin() + 4, received.begin())) {
        error_ = BinaryError::invalid_header;
        return false;
    }
    if (received[4] != header[4] || received[5] != header[5]) {
        error_ = BinaryError::unsupported_version;
        return false;
    }
    if (received[6] != 0 || received[7] != 0) {
        error_ = BinaryError::invalid_header;
        return false;
    }
    headerRead_ = true;
    nextOffset_ = header.size();
    return true;
}

BinaryReadStatus BinaryReader::read(CycleRecord& record)
{
    if (error_ != BinaryError::none) {
        return BinaryReadStatus::error;
    }
    if (finished_) {
        return BinaryReadStatus::end;
    }
    if (!headerRead_ && !readHeader()) {
        return BinaryReadStatus::error;
    }

    offset_ = nextOffset_;
    Frame frame{};
    const auto lengthStatus = readBytes(input_, frame.data(), lengthSize);
    if (lengthStatus == ChunkStatus::end) {
        finished_ = true;
        return BinaryReadStatus::end;
    }
    if (lengthStatus != ChunkStatus::complete) {
        error_ = chunkError(lengthStatus);
        return BinaryReadStatus::error;
    }
    ByteReader length(frame.data(), lengthSize);
    std::uint16_t payloadSize = 0;
    if (!length.integer(payloadSize) || payloadSize < minPayloadSize || payloadSize > maxPayloadSize) {
        error_ = BinaryError::invalid_length;
        return BinaryReadStatus::error;
    }
    const auto protectedSize = lengthSize + payloadSize;
    const auto payloadStatus = readBytes(input_, frame.data() + lengthSize, payloadSize + crcSize);
    if (payloadStatus != ChunkStatus::complete) {
        error_ = chunkError(payloadStatus);
        return BinaryReadStatus::error;
    }
    ByteReader trailer(frame.data() + protectedSize, crcSize);
    std::uint32_t checksum = 0;
    if (!trailer.integer(checksum) || checksum != crc32(frame.data(), protectedSize)) {
        error_ = BinaryError::checksum_mismatch;
        return BinaryReadStatus::error;
    }

    CycleRecord decoded;
    if (!decodePayload(frame.data() + lengthSize, payloadSize, decoded)) {
        error_ = BinaryError::invalid_record;
        return BinaryReadStatus::error;
    }
    if (lastCycle_ && decoded.cycle <= *lastCycle_) {
        error_ = BinaryError::cycle_order;
        return BinaryReadStatus::error;
    }
    record = decoded;
    lastCycle_ = decoded.cycle;
    nextOffset_ += protectedSize + crcSize;
    return BinaryReadStatus::record;
}

}  // namespace ecu
