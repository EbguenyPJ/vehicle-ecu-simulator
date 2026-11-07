#include "ecu/dashboard.hpp"

#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#include "braille.hpp"

namespace ecu {
namespace {

constexpr std::array<std::size_t, 6> columnWidths{11, 10, 4, 7, 11, 11};
constexpr std::string_view spaces = "                           ";

std::string_view signalName(SignalId id) noexcept
{
    switch (id) {
    case SignalId::speed: return "Velocidad";
    case SignalId::engine_rpm: return "RPM";
    case SignalId::coolant_temperature: return "Temperatura";
    case SignalId::throttle_position: return "Acelerador";
    case SignalId::battery_voltage: return "Bateria";
    case SignalId::oil_pressure: return "Aceite";
    }
    return "Desconocida";
}

std::string_view qualityName(SampleQuality quality) noexcept
{
    switch (quality) {
    case SampleQuality::in_range: return "VALIDA";
    case SampleQuality::out_of_range: return "FUERA_RANGO";
    case SampleQuality::non_finite: return "NO_FINITA";
    }
    return "DESCONOCIDA";
}

std::string_view statusName(SignalStatus status) noexcept
{
    switch (status) {
    case SignalStatus::valid: return "VALID";
    case SignalStatus::invalid: return "INVALID";
    case SignalStatus::missing: return "MISSING";
    }
    return "DESCONOCIDO";
}

std::string_view freshnessName(Freshness freshness) noexcept
{
    switch (freshness) {
    case Freshness::never_received: return "NO_RECIBIDA";
    case Freshness::fresh: return "FRESCA";
    case Freshness::retained: return "RETENIDA";
    case Freshness::expired: return "CADUCADA";
    }
    return "DESCONOCIDA";
}

std::string_view stateName(ControlState state) noexcept
{
    switch (state) {
    case ControlState::init: return "INIT";
    case ControlState::self_test: return "SELF_TEST";
    case ControlState::operational: return "OPERATIONAL";
    case ControlState::degraded: return "DEGRADED";
    case ControlState::safe_state: return "SAFE_STATE";
    case ControlState::shutdown: return "SHUTDOWN";
    }
    return "DESCONOCIDO";
}

std::string_view actionName(ControlAction action) noexcept
{
    switch (action) {
    case ControlAction::inhibit_output: return "INHIBIR";
    case ControlAction::limited_operation: return "LIMITADA";
    case ControlAction::normal_operation: return "NORMAL";
    }
    return "DESCONOCIDA";
}

std::string_view diagnosticName(DiagnosticCode code) noexcept
{
    switch (code) {
    case DiagnosticCode::signal_missing: return "SENAL_AUSENTE";
    case DiagnosticCode::signal_expired: return "SENAL_CADUCADA";
    case DiagnosticCode::signal_retained: return "SENAL_RETENIDA";
    case DiagnosticCode::signal_out_of_range: return "MEDICION_FUERA_RANGO";
    case DiagnosticCode::signal_non_finite: return "MEDICION_NO_FINITA";
    case DiagnosticCode::coolant_over_temperature: return "SOBRE_TEMPERATURA";
    case DiagnosticCode::low_oil_pressure: return "ACEITE_BAJO";
    case DiagnosticCode::battery_voltage_outside_operating_range: return "BATERIA_FUERA_LIMITE";
    case DiagnosticCode::powertrain_incoherence: return "MARCHA_INCOHERENTE";
    case DiagnosticCode::recovery_pending: return "RECUPERACION_PENDIENTE";
    }
    return "DESCONOCIDO";
}

std::string formatValue(double value)
{
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return std::signbit(value) ? "-inf" : "+inf";
    }

    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(6) << value;
    auto formatted = text.str();
    if (formatted.size() > columnWidths[1]) {
        text.str({});
        text << std::scientific << std::setprecision(2) << value;
        formatted = text.str();
    }
    return formatted;
}

void writeRow(std::ostream& out, const std::array<std::string_view, 7>& fields)
{
    for (std::size_t index = 0; index < columnWidths.size(); ++index) {
        const auto field = fields[index];
        out << field;
        if (field.size() < columnWidths[index]) {
            out << spaces.substr(0, columnWidths[index] - field.size());
        }
        out << ' ';
    }
    out << fields.back() << '\n';
}

void writeSignals(std::ostream& out, const SignalSnapshot& snapshot, bool compact)
{
    if (!compact) {
        writeRow(out, {"Senal", "Valor", "Unid", "Gateway", "Calidad", "Frescura", "Edad"});
    }

    for (const auto& descriptor : signalCatalog) {
        const auto observation = snapshot.observe(descriptor.id).value_or(SignalObservation{});
        const auto value = observation.sample ? formatValue(observation.sample->value) : "---";
        const auto quality = observation.sample ? qualityName(observation.sample->quality) : "SIN_MUESTRA";
        const auto status = statusName(observation.status);
        const auto freshness = freshnessName(observation.freshness);
        const auto age = observation.age ? std::to_string(*observation.age) : "-";
        const auto name = signalName(descriptor.id);

        if (compact) {
            out << "Senal: " << name << '\n'
                << "  Valor: " << value << ' ' << descriptor.unit << '\n'
                << "  Resultado: " << status << '\n'
                << "  Calidad: " << quality << '\n'
                << "  Frescura: " << freshness << '\n'
                << "  Edad: " << age << '\n';
        } else {
            writeRow(out, {name, value, descriptor.unit, status, quality, freshness, age});
        }
    }
}

std::optional<double> dialValue(const SignalSnapshot& snapshot, SignalId id)
{
    const auto observation = snapshot.observe(id);
    if (!observation || observation->freshness != Freshness::fresh || !observation->sample ||
        observation->sample->quality != SampleQuality::in_range) {
        return std::nullopt;
    }
    return observation->sample->value;
}

void writeCentered(std::ostream& out, std::string_view text)
{
    constexpr auto width = detail::brailleDialWidth;
    const auto padding = text.size() < width ? width - text.size() : 0;
    const auto left = padding / 2;
    out << spaces.substr(0, left) << text << spaces.substr(0, padding - left);
}

void writeScale(std::ostream& out, std::string_view minimum, std::string_view maximum)
{
    static_assert(2 * columnWidths[1] < detail::brailleDialWidth);
    out << minimum << spaces.substr(0, detail::brailleDialWidth - minimum.size() - maximum.size()) << maximum;
}

void writeDials(std::ostream& out, const SignalSnapshot& snapshot)
{
    constexpr auto speedIndex = static_cast<std::size_t>(SignalId::speed);
    constexpr auto rpmIndex = static_cast<std::size_t>(SignalId::engine_rpm);
    const auto speed = dialValue(snapshot, SignalId::speed);
    const auto rpm = dialValue(snapshot, SignalId::engine_rpm);
    const auto& speedScale = signalCatalog[speedIndex];
    const auto& rpmScale = signalCatalog[rpmIndex];
    // Los límites del catálogo inmutable se formatean una sola vez.
    static const auto speedMinimum = formatValue(speedScale.minimum);
    static const auto speedMaximum = formatValue(speedScale.maximum);
    static const auto rpmMinimum = formatValue(rpmScale.minimum);
    static const auto rpmMaximum = formatValue(rpmScale.maximum);
    const auto fraction = [](const std::optional<double>& value, const SignalDescriptor& scale)
        -> std::optional<double> {
        if (!value) {
            return std::nullopt;
        }
        return (*value - scale.minimum) / (scale.maximum - scale.minimum);
    };
    const auto speedDial = detail::drawBrailleDial(fraction(speed, speedScale));
    const auto rpmDial = detail::drawBrailleDial(fraction(rpm, rpmScale));

    constexpr std::string_view gap = "    ";
    writeCentered(out, "VELOCIDAD");
    out << gap;
    writeCentered(out, "RPM");
    out << '\n';
    for (std::size_t row = 0; row < detail::brailleDialHeight; ++row) {
        out << speedDial[row] << gap << rpmDial[row] << '\n';
    }
    writeScale(out, speedMinimum, speedMaximum);
    out << gap;
    writeScale(out, rpmMinimum, rpmMaximum);
    out << '\n';
    const auto caption = [](const std::optional<double>& value, const SignalDescriptor& scale) {
        return (value ? formatValue(*value) : "---") + ' ' + std::string(scale.unit);
    };
    writeCentered(out, caption(speed, speedScale));
    out << gap;
    writeCentered(out, caption(rpm, rpmScale));
    out << "\n\n";
}

void writeDiagnostic(std::ostream& out, const Diagnostic& diagnostic)
{
    out << diagnosticName(diagnostic.code);
    if (diagnostic.signal) {
        out << " [" << signalName(*diagnostic.signal) << ']';
    } else if (diagnostic.code == DiagnosticCode::powertrain_incoherence) {
        out << " [Velocidad/RPM/Acelerador]";
    }
}

void writeControl(std::ostream& out, const ControlResult& control)
{
    out << "\nControl: " << stateName(control.state)
        << "\nAccion: " << actionName(control.action) << '\n';
    if (control.diagnostics.size() == 0) {
        out << "Diagnostico actual: NINGUNO\n";
    } else {
        out << "Diagnostico actual:\n";
        for (const auto& diagnostic : control.diagnostics) {
            out << (diagnostic.severity == Severity::critical ? "  [CRITICO] " : "  [DEGRADANTE] ");
            writeDiagnostic(out, diagnostic);
            out << '\n';
        }
    }
    out << "Causa enclavada: ";
    if (control.latchedFault) {
        writeDiagnostic(out, control.latchedFault->cause);
        out << "\nCiclo de enclavamiento: " << std::to_string(control.latchedFault->cycle) << '\n';
    } else {
        out << "NINGUNA\n";
    }
}

}  // namespace

DashboardFormat selectDashboardFormat(bool utf8Terminal, std::size_t columns) noexcept
{
    return utf8Terminal && columns >= dashboardMinBrailleColumns
        ? DashboardFormat::braille : DashboardFormat::ascii;
}

bool renderDashboard(std::ostream& out, const SignalSnapshot& snapshot, const ControlResult& control,
                     DashboardFormat format, std::size_t columns)
{
    if (!out || snapshot.cycle() != control.cycle) {
        return false;
    }

    out << "VEHICLE ECU SIMULATOR | CICLO " << std::to_string(snapshot.cycle()) << '\n';
    if (format == DashboardFormat::braille) {
        writeDials(out, snapshot);
    }
    writeSignals(out, snapshot, columns != 0 && columns < dashboardMinBrailleColumns);
    writeControl(out, control);
    out << '\n';
    return static_cast<bool>(out);
}

}  // namespace ecu
