#include "ecu/dashboard.hpp"

#include <algorithm>
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

    const auto invalid = snapshot.count(SignalStatus::invalid);
    const auto missing = snapshot.count(SignalStatus::missing);
    const auto valid = signalCount - invalid - missing;
    out << "Resumen Gateway: VALID=" << std::to_string(valid)
        << " INVALID=" << std::to_string(invalid)
        << " MISSING=" << std::to_string(missing) << '\n';
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

std::optional<double> scaleFraction(const std::optional<double>& value, const SignalDescriptor& scale)
{
    if (!value) {
        return std::nullopt;
    }
    return (*value - scale.minimum) / (scale.maximum - scale.minimum);
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
    const auto speedDial = detail::drawBrailleDial(scaleFraction(speed, speedScale));
    const auto rpmDial = detail::drawBrailleDial(scaleFraction(rpm, rpmScale));

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

// Cada glifo del panel ocupa una columna en una fuente monoespaciada.
constexpr std::size_t panelWidth = dashboardMinBrailleColumns - 3;
constexpr std::size_t dialGap = panelWidth - 2 * detail::brailleDialWidth;
constexpr std::size_t gaugeWidth = 14;
constexpr std::string_view reset = "\033[0m";
constexpr std::string_view dim = "\033[90m";
constexpr std::string_view green = "\033[92m";
constexpr std::string_view amber = "\033[93m";
constexpr std::string_view red = "\033[91m";

std::string_view signalColor(const SignalObservation& observation) noexcept
{
    if (observation.status == SignalStatus::invalid) {
        return red;
    }
    return observation.freshness == Freshness::fresh ? green : amber;
}

std::string_view stateBadge(ControlState state) noexcept
{
    switch (state) {
    case ControlState::init: return "\033[97;100m";
    case ControlState::self_test: return "\033[97;44m";
    case ControlState::operational: return "\033[30;102m";
    case ControlState::degraded: return "\033[30;103m";
    case ControlState::safe_state: return "\033[97;41m";
    case ControlState::shutdown: return "\033[97;45m";
    }
    return reset;
}

void writePanelLine(std::ostream& out, std::string_view line)
{
    out << dim << u8"\u2502" << reset << line << std::string(panelWidth - line.size(), ' ')
        << dim << u8"\u2502" << reset << '\n';
}

void writePanelBorder(std::ostream& out, std::string_view left, std::string_view right)
{
    out << dim << left;
    for (std::size_t column = 0; column < panelWidth; ++column) {
        out << u8"\u2500";
    }
    out << right << reset << '\n';
}

// Las filas de instrumentos usan anchos fijos y sus secuencias SGR ocupan cero columnas.
void writeInstrumentRow(std::ostream& out, std::string_view left, std::string_view center,
                        std::string_view right, std::string_view leftColor = reset,
                        std::string_view centerColor = reset, std::string_view rightColor = reset)
{
    out << dim << u8"\u2502" << leftColor << left
        << std::string((dialGap - center.size()) / 2, ' ')
        << centerColor << center
        << std::string((dialGap - center.size() + 1) / 2, ' ')
        << rightColor << right << dim << u8"\u2502" << reset << '\n';
}

std::string dialCaption(const SignalObservation& observation, std::string_view unit)
{
    const auto value = observation.sample ? formatValue(observation.sample->value) : "---";
    return value + ' ' + std::string(unit) + ' ' + std::string(statusName(observation.status));
}

void writeLiveDials(std::ostream& out, const SignalSnapshot& snapshot)
{
    const auto& speed = signalCatalog[static_cast<std::size_t>(SignalId::speed)];
    const auto& rpm = signalCatalog[static_cast<std::size_t>(SignalId::engine_rpm)];
    const auto speedObservation = *snapshot.observe(speed.id);
    const auto rpmObservation = *snapshot.observe(rpm.id);
    const auto speedDial = detail::drawBrailleDial(scaleFraction(dialValue(snapshot, speed.id), speed));
    const auto rpmDial = detail::drawBrailleDial(scaleFraction(dialValue(snapshot, rpm.id), rpm));
    const auto invalid = snapshot.count(SignalStatus::invalid);
    const auto missing = snapshot.count(SignalStatus::missing);
    const auto valid = signalCount - invalid - missing;
    const auto warningColor = invalid != 0 ? red : missing != 0 ? amber : green;
    const std::array<std::string, detail::brailleDialHeight> center{
        "", invalid + missing == 0 ? "EN ORDEN" : "ATENCION", "",
        "VALID   " + std::to_string(valid), "INVALID " + std::to_string(invalid),
        "MISSING " + std::to_string(missing), "",
    };

    const auto centered = [](std::string_view text) {
        std::ostringstream line;
        writeCentered(line, text);
        return line.str();
    };
    writeInstrumentRow(out, centered("VELOCIDAD"), "GATEWAY", centered("RPM"));
    for (std::size_t row = 0; row < speedDial.size(); ++row) {
        writeInstrumentRow(out, speedDial[row], center[row], rpmDial[row],
                           signalColor(speedObservation), warningColor, signalColor(rpmObservation));
    }
    std::ostringstream speedScale;
    std::ostringstream rpmScale;
    writeScale(speedScale, formatValue(speed.minimum), formatValue(speed.maximum));
    writeScale(rpmScale, formatValue(rpm.minimum), formatValue(rpm.maximum));
    writeInstrumentRow(out, speedScale.str(), "", rpmScale.str(), dim, reset, dim);
    writeInstrumentRow(out, centered(dialCaption(speedObservation, speed.unit)), "",
                       centered(dialCaption(rpmObservation, rpm.unit)),
                       signalColor(speedObservation), reset, signalColor(rpmObservation));
}

void writeLiveGauge(std::ostream& out, const SignalSnapshot& snapshot, const SignalDescriptor& signal)
{
    const auto observation = *snapshot.observe(signal.id);
    const auto color = signalColor(observation);
    std::size_t filled = 0;
    if (observation.sample && std::isfinite(observation.sample->value) &&
        observation.status != SignalStatus::missing) {
        const auto value = std::clamp(observation.sample->value, signal.minimum, signal.maximum);
        const auto fraction = (value - signal.minimum) / (signal.maximum - signal.minimum);
        filled = static_cast<std::size_t>(std::lround(fraction * static_cast<double>(gaugeWidth)));
    }
    const auto name = signalName(signal.id);
    std::ostringstream value;
    value << (observation.sample ? formatValue(observation.sample->value) : "---") << ' ' << signal.unit;
    const auto reading = value.str();
    const auto status = statusName(observation.status);
    const auto age = observation.age ? std::to_string(*observation.age) : "-";
    const auto freshness = freshnessName(observation.freshness);
    constexpr std::size_t tailWidth = 24;
    const auto freshnessWidth = freshness.size() + 1 + age.size() <= tailWidth ? freshness.size() : 3;
    const auto tail = std::string(freshness.substr(0, freshnessWidth)) + ' ' + age;
    out << dim << u8"\u2502" << reset << ' ' << name << spaces.substr(0, 12 - name.size()) << ' ' << color;
    for (std::size_t cell = 0; cell < gaugeWidth; ++cell) {
        out << (cell < filled ? u8"\u2588" : u8"\u2591");
    }
    out << reset << ' ' << reading << spaces.substr(0, 15 - reading.size()) << ' '
        << color << status << reset << spaces.substr(0, 7 - status.size()) << ' '
        << tail << spaces.substr(0, tailWidth - tail.size()) << dim << u8"\u2502" << reset << '\n';
}

void writeLiveControl(std::ostream& out, const ControlResult& control)
{
    const auto state = stateName(control.state);
    const auto action = actionName(control.action);
    constexpr std::string_view label = " ECU DE CONTROL  ";
    constexpr std::string_view actionLabel = "  Accion ";
    out << dim << u8"\u2502" << reset << label << stateBadge(control.state) << ' ' << state << ' ' << reset
        << actionLabel << action
        << std::string(panelWidth - label.size() - state.size() - 2 - actionLabel.size() - action.size(), ' ')
        << dim << u8"\u2502" << reset << '\n';

    std::ostringstream current;
    current << " Diagnostico: ";
    if (const auto primary = control.diagnostics.primary()) {
        writeDiagnostic(current, *primary);
        if (control.diagnostics.size() > 1) {
            current << " (+" << control.diagnostics.size() - 1 << ')';
        }
    } else {
        current << "NINGUNO";
    }
    writePanelLine(out, current.str());

    std::ostringstream latched;
    latched << " Enclavada: ";
    if (control.latchedFault) {
        writeDiagnostic(latched, control.latchedFault->cause);
    } else {
        latched << "NINGUNA";
    }
    writePanelLine(out, latched.str());
    writePanelLine(out, control.latchedFault
        ? " Ciclo de enclavamiento: " + std::to_string(control.latchedFault->cycle) : "");
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

bool renderLiveDashboard(std::ostream& out, const SignalSnapshot& snapshot, const ControlResult& control)
{
    if (!out || snapshot.cycle() != control.cycle) {
        return false;
    }

    writePanelBorder(out, u8"\u256d", u8"\u256e");
    constexpr std::string_view title = " ECU GATEWAY / CONTROL";
    const auto cycle = "CICLO " + std::to_string(snapshot.cycle()) + ' ';
    writePanelLine(out, std::string(title) + std::string(panelWidth - title.size() - cycle.size(), ' ') + cycle);
    writeLiveDials(out, snapshot);
    writePanelBorder(out, u8"\u251c", u8"\u2524");
    for (const auto& signal : signalCatalog) {
        if (signal.id != SignalId::speed && signal.id != SignalId::engine_rpm) {
            writeLiveGauge(out, snapshot, signal);
        }
    }
    writePanelBorder(out, u8"\u251c", u8"\u2524");
    writeLiveControl(out, control);
    writePanelBorder(out, u8"\u2570", u8"\u256f");
    return static_cast<bool>(out);
}

}  // namespace ecu
