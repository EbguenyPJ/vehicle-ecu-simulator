#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string_view>

#include "ecu/record.hpp"

namespace ecu {

enum class BinaryError {
    none,
    io_error,
    invalid_header,
    unsupported_version,
    invalid_length,
    truncated,
    checksum_mismatch,
    invalid_record,
    cycle_order,
    closed,
};

[[nodiscard]] std::string_view binaryErrorMessage(BinaryError error) noexcept;

class BinaryWriter {
public:
    explicit BinaryWriter(std::ostream& output) noexcept : output_(output) {}
    BinaryWriter(const BinaryWriter&) = delete;
    BinaryWriter& operator=(const BinaryWriter&) = delete;

    [[nodiscard]] bool write(const CycleRecord& record);
    // Escribe la cabecera si hace falta y vacía el flujo del llamador.
    [[nodiscard]] bool finish();
    [[nodiscard]] BinaryError error() const noexcept { return error_; }

private:
    [[nodiscard]] bool writeHeader();
    [[nodiscard]] bool writeBytes(const std::uint8_t* bytes, std::size_t size);

    std::ostream& output_;
    std::optional<Cycle> lastCycle_{};
    BinaryError error_{BinaryError::none};
    bool headerWritten_{false};
    bool finished_{false};
};

enum class BinaryReadStatus { record, end, error };

class BinaryReader {
public:
    explicit BinaryReader(std::istream& input) noexcept : input_(input) {}
    BinaryReader(const BinaryReader&) = delete;
    BinaryReader& operator=(const BinaryReader&) = delete;

    // Solo reemplaza record tras validar el registro completo.
    [[nodiscard]] BinaryReadStatus read(CycleRecord& record);
    [[nodiscard]] BinaryError error() const noexcept { return error_; }
    // Posición del encabezado o registro leído, contando desde el byte cero.
    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }

private:
    [[nodiscard]] bool readHeader();

    std::istream& input_;
    std::optional<Cycle> lastCycle_{};
    BinaryError error_{BinaryError::none};
    std::uint64_t offset_{0};
    std::uint64_t nextOffset_{0};
    bool headerRead_{false};
    bool finished_{false};
};

}  // namespace ecu
