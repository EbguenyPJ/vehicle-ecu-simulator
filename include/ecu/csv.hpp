#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string_view>

#include "ecu/record.hpp"

namespace ecu {

enum class CsvError {
    none,
    io_error,
    invalid_header,
    unsupported_version,
    invalid_record,
    cycle_order,
    line_too_long,
    closed,
};

[[nodiscard]] std::string_view csvErrorMessage(CsvError error) noexcept;

class CsvWriter {
public:
    explicit CsvWriter(std::ostream& output) noexcept : output_(output) {}
    CsvWriter(const CsvWriter&) = delete;
    CsvWriter& operator=(const CsvWriter&) = delete;

    [[nodiscard]] bool write(const CycleRecord& record);
    // Emite la cabecera si el archivo está vacío y vacía el flujo.
    [[nodiscard]] bool finish();
    [[nodiscard]] CsvError error() const noexcept { return error_; }

private:
    [[nodiscard]] bool writeHeader();
    [[nodiscard]] bool writeLine(std::string_view line);

    std::ostream& output_;
    std::optional<Cycle> lastCycle_{};
    CsvError error_{CsvError::none};
    bool headerWritten_{false};
    bool finished_{false};
};

enum class CsvReadStatus { record, end, error };

class CsvReader {
public:
    explicit CsvReader(std::istream& input) noexcept : input_(input) {}
    CsvReader(const CsvReader&) = delete;
    CsvReader& operator=(const CsvReader&) = delete;

    // Solo modifica record cuando devuelve CsvReadStatus::record.
    [[nodiscard]] CsvReadStatus read(CycleRecord& record);
    [[nodiscard]] CsvError error() const noexcept { return error_; }
    [[nodiscard]] std::size_t line() const noexcept { return line_; }

private:
    std::istream& input_;
    std::optional<Cycle> lastCycle_{};
    CsvError error_{CsvError::none};
    std::size_t line_{0};
    bool headerRead_{false};
    bool finished_{false};
};

}  // namespace ecu
