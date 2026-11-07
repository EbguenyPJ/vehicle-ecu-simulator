#include "ecu/dashboard.hpp"
#include "ecu/simulation.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace ecu;

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void expectText(std::string_view frame, std::string_view expected)
{
    if (frame.find(expected) == std::string_view::npos) {
        throw std::runtime_error("missing dashboard text: " + std::string(expected));
    }
}

constexpr std::array<std::string_view, 6> signalNames{
    "Velocidad", "RPM", "Temperatura", "Acelerador", "Bateria", "Aceite",
};

std::string_view rowFor(std::string_view frame, std::string_view signal)
{
    while (!frame.empty()) {
        const auto end = frame.find('\n');
        const auto line = frame.substr(0, end);
        if (line.substr(0, signal.size()) == signal && line.size() > signal.size() &&
            line[signal.size()] == ' ') {
            return line;
        }
        if (end == std::string_view::npos) {
            break;
        }
        frame.remove_prefix(end + 1);
    }
    throw std::runtime_error("missing signal row: " + std::string(signal));
}

struct SignalColumns {
    std::string value;
    std::string unit;
    std::string status;
    std::string quality;
    std::string freshness;
    std::string age;
};

SignalColumns columnsFor(std::string_view frame, std::string_view signal)
{
    std::istringstream row{std::string(rowFor(frame, signal))};
    std::string label;
    SignalColumns columns;
    require(static_cast<bool>(row >> label >> columns.value >> columns.unit >>
                              columns.status >> columns.quality >> columns.freshness >> columns.age),
            "a dashboard row must contain every signal column");
    return columns;
}

void expectSignal(std::string_view frame, std::string_view signal, std::string_view status,
                  std::string_view quality, std::string_view freshness, std::string_view age)
{
    const auto columns = columnsFor(frame, signal);
    if (columns.status != status || columns.quality != quality ||
        columns.freshness != freshness || columns.age != age) {
        throw std::runtime_error("unexpected status, quality, freshness or age in row: " +
                                 std::string(rowFor(frame, signal)));
    }
}

std::string render(const SignalSnapshot& snapshot, const ControlResult& result,
                   DashboardFormat format, std::size_t columns = dashboardMinBrailleColumns)
{
    std::ostringstream out;
    require(renderDashboard(out, snapshot, result, format, columns),
            "a coherent cycle must render successfully");
    return out.str();
}

struct Tick {
    SignalSnapshot snapshot;
    ControlResult result;
};

Tick process(Gateway& gateway, ControlECU& control, Cycle cycle, const SignalBatch& batch,
             ControlEvent event = ControlEvent::none)
{
    const auto snapshot = gateway.processCycle(cycle, batch);
    require(snapshot.has_value(), "Gateway must publish an increasing cycle");
    const auto result = control.step(*snapshot, event);
    require(result.has_value(), "Control must process the published cycle");
    return {*snapshot, *result};
}

void ascii()
{
    Gateway gateway;
    ControlECU control;
    ControlResult initial;
    const auto initialFrame = render(gateway.snapshot(), initial, DashboardFormat::ascii);
    expectText(initialFrame, "Control: INIT");
    expectText(initialFrame, "Accion: INHIBIR");
    expectText(initialFrame, "Gateway");
    for (const auto name : signalNames) {
        expectSignal(initialFrame, name, "MISSING", "SIN_MUESTRA", "NO_RECIBIDA", "-");
    }
    SignalBatch batch;
    const double infinity = std::numeric_limits<double>::infinity();
    batch.add(SignalId::speed, 0.0);
    batch.add(SignalId::engine_rpm, -std::numeric_limits<double>::max());
    batch.add(SignalId::coolant_temperature, std::numeric_limits<double>::quiet_NaN());
    batch.add(SignalId::throttle_position, infinity);
    batch.add(SignalId::battery_voltage, -infinity);

    const auto tick = process(gateway, control, 1, batch);
    const auto frame = render(tick.snapshot, tick.result, DashboardFormat::ascii);
    expectText(frame, "VEHICLE ECU SIMULATOR | CICLO 1");
    expectText(frame, "Control: SAFE_STATE");
    expectText(frame, "Accion: INHIBIR");
    require(std::all_of(frame.begin(), frame.end(), [](unsigned char byte) {
        return byte == '\n' || (byte >= 32 && byte <= 126);
    }), "ASCII output must contain printable ASCII and newlines only");

    expectSignal(frame, "Velocidad", "VALID", "VALIDA", "FRESCA", "0");
    expectSignal(frame, "RPM", "INVALID", "FUERA_RANGO", "FRESCA", "0");
    expectSignal(frame, "Temperatura", "INVALID", "NO_FINITA", "FRESCA", "0");
    expectSignal(frame, "Acelerador", "INVALID", "NO_FINITA", "FRESCA", "0");
    expectSignal(frame, "Bateria", "INVALID", "NO_FINITA", "FRESCA", "0");
    expectSignal(frame, "Aceite", "MISSING", "SIN_MUESTRA", "NO_RECIBIDA", "-");
    require(columnsFor(frame, "Velocidad").value == "0" &&
                columnsFor(frame, "Velocidad").unit == "km/h" &&
                columnsFor(frame, "RPM").unit == "rpm" &&
                columnsFor(frame, "Aceite").value == "---",
            "zero, units and missing values must be distinct");
    require(columnsFor(frame, "Temperatura").value == "nan" &&
                columnsFor(frame, "Acelerador").value == "+inf" &&
                columnsFor(frame, "Bateria").value == "-inf",
            "the three non-finite values must be distinguishable");
    require(rowFor(frame, "RPM").size() <= dashboardMinBrailleColumns,
            "even the maximum finite reading must fit in a wide row");
    const auto lastCycle = std::numeric_limits<Cycle>::max();
    const auto aged = process(gateway, control, lastCycle, SignalBatch{});
    const auto agedFrame = render(aged.snapshot, aged.result, DashboardFormat::ascii);
    expectSignal(agedFrame, "RPM", "MISSING", "FUERA_RANGO", "CADUCADA", std::to_string(lastCycle - 1));
    expectSignal(agedFrame, "Temperatura", "MISSING", "NO_FINITA", "CADUCADA", std::to_string(lastCycle - 1));
    require(rowFor(agedFrame, "RPM").size() <= dashboardMinBrailleColumns,
            "the largest age and a large sample must share a wide row");

    const auto narrow = render(tick.snapshot, tick.result, DashboardFormat::ascii, 48);
    expectText(narrow, "Senal: Velocidad");
    expectText(narrow, "  Valor: 0");
    expectText(narrow, "  Resultado: VALID");
    expectText(narrow, "  Calidad: VALIDA");
    expectText(narrow, "  Frescura: FRESCA");
    expectText(narrow, "  Edad: 0");
    expectText(narrow, "Senal: Aceite");
    expectText(narrow, "  Resultado: INVALID");
    expectText(narrow, "  Resultado: MISSING");
    expectText(narrow, "  Calidad: SIN_MUESTRA");
    expectText(narrow, "  Frescura: NO_RECIBIDA");
    require(selectDashboardFormat(false, 120) == DashboardFormat::ascii &&
                selectDashboardFormat(true, 0) == DashboardFormat::ascii &&
                selectDashboardFormat(true, dashboardMinBrailleColumns - 1) == DashboardFormat::ascii &&
                selectDashboardFormat(true, dashboardMinBrailleColumns) == DashboardFormat::braille,
            "automatic selection needs both UTF-8 terminal support and sufficient width");

    auto mismatched = tick.result;
    ++mismatched.cycle;
    std::ostringstream mismatchOutput;
    mismatchOutput << "unchanged";
    require(!renderDashboard(mismatchOutput, tick.snapshot, mismatched, DashboardFormat::ascii) &&
                mismatchOutput.str() == "unchanged",
            "different cycles must be rejected without writing");
    std::ostringstream failedOutput;
    failedOutput.setstate(std::ios::badbit);
    require(!renderDashboard(failedOutput, tick.snapshot, tick.result, DashboardFormat::ascii),
            "a failed stream must be reported");

    Gateway extremeGateway;
    ControlECU extremeControl;
    SignalBatch extremeBatch;
    extremeBatch.add(SignalId::coolant_temperature, -std::numeric_limits<double>::max());
    process(extremeGateway, extremeControl, 1, extremeBatch);
    const auto extreme = process(extremeGateway, extremeControl, lastCycle, SignalBatch{});
    const auto extremeFrame = render(extreme.snapshot, extreme.result, DashboardFormat::ascii);
    expectSignal(extremeFrame, "Temperatura", "MISSING", "FUERA_RANGO", "CADUCADA",
                 std::to_string(lastCycle - 1));
    require(columnsFor(extremeFrame, "Temperatura").value == "-1.80e+308" &&
                rowFor(extremeFrame, "Temperatura").size() <= dashboardMinBrailleColumns,
            "the longest label, largest finite value and largest age must fit in 80 columns");
    const auto original = extreme.snapshot.observe(SignalId::coolant_temperature);
    require(original && original->sample &&
                original->sample->value == -std::numeric_limits<double>::max(),
            "presentation rounding must not change the stored sample");
}

std::string brailleGlyphs(std::string_view frame)
{
    std::string glyphs;
    for (std::size_t offset = 0; offset + 2 < frame.size(); ++offset) {
        const auto first = static_cast<unsigned char>(frame[offset]);
        const auto second = static_cast<unsigned char>(frame[offset + 1]);
        const auto third = static_cast<unsigned char>(frame[offset + 2]);
        if (first == 0xe2 && second >= 0xa0 && second <= 0xa3 && third >= 0x80 && third <= 0xbf) {
            glyphs.append(frame.substr(offset, 3));
            offset += 2;
        }
    }
    return glyphs;
}

std::string_view tableAndControl(std::string_view frame)
{
    const auto table = frame.find("Senal");
    require(table != std::string_view::npos, "a wide dashboard needs a signal table");
    return frame.substr(table);
}

SignalBatch auxiliaryReadings()
{
    SignalBatch batch;
    batch.add(SignalId::coolant_temperature, 90.0);
    batch.add(SignalId::throttle_position, 50.0);
    batch.add(SignalId::battery_voltage, 13.8);
    batch.add(SignalId::oil_pressure, 2.5);
    return batch;
}

void braille()
{
    Gateway gateway;
    ControlECU control;
    const auto auxiliary = auxiliaryReadings();
    const auto emptyDials = process(gateway, control, 1, auxiliary);
    const auto empty = render(emptyDials.snapshot, emptyDials.result, DashboardFormat::braille);
    const auto arc = brailleGlyphs(empty);
    require(!arc.empty(), "even without a reading, the Braille dial arcs must be visible");
    expectText(empty, "VELOCIDAD");
    expectText(empty, "RPM");
    expectText(empty, "Control: SELF_TEST");
    expectText(empty, "Accion: INHIBIR");
    expectSignal(empty, "Velocidad", "MISSING", "SIN_MUESTRA", "NO_RECIBIDA", "-");

    auto lowerReadings = auxiliary;
    lowerReadings.add(SignalId::speed, 40.0);
    lowerReadings.add(SignalId::engine_rpm, 1800.0);
    const auto lower = process(gateway, control, 2, lowerReadings);
    const auto lowDial = render(lower.snapshot, lower.result, DashboardFormat::braille);
    expectSignal(lowDial, "Velocidad", "VALID", "VALIDA", "FRESCA", "0");
    require(brailleGlyphs(lowDial) != arc, "fresh valid samples must draw a needle");

    auto higherReadings = auxiliary;
    higherReadings.add(SignalId::speed, 100.0);
    higherReadings.add(SignalId::engine_rpm, 4500.0);
    const auto higher = process(gateway, control, 3, higherReadings);
    const auto highDial = render(higher.snapshot, higher.result, DashboardFormat::braille);
    require(brailleGlyphs(highDial) != brailleGlyphs(lowDial),
            "the rendered needles must move as the readings change");
    require(highDial.find('\x1b') == std::string::npos,
            "a Braille frame must not depend on terminal escapes");
    require(tableAndControl(highDial) ==
                tableAndControl(render(higher.snapshot, higher.result, DashboardFormat::ascii)),
            "both formats must display the same signal and control data");

    const auto retained = process(gateway, control, 4, auxiliary);
    const auto retainedFrame = render(retained.snapshot, retained.result, DashboardFormat::braille);
    expectSignal(retainedFrame, "Velocidad", "VALID", "VALIDA", "RETENIDA", "1");
    require(brailleGlyphs(retainedFrame) == arc,
            "retained readings must not move the needles");
    const auto expired = process(gateway, control, 7, auxiliary);
    const auto expiredFrame = render(expired.snapshot, expired.result, DashboardFormat::braille);
    expectSignal(expiredFrame, "Velocidad", "MISSING", "VALIDA", "CADUCADA", "4");
    require(brailleGlyphs(expiredFrame) == arc,
            "expired readings must not move the needles");

    auto invalidReadings = auxiliary;
    invalidReadings.add(SignalId::speed, 500.0);
    invalidReadings.add(SignalId::engine_rpm, std::numeric_limits<double>::infinity());
    const auto invalid = process(gateway, control, 8, invalidReadings);
    const auto invalidFrame = render(invalid.snapshot, invalid.result, DashboardFormat::braille);
    expectSignal(invalidFrame, "Velocidad", "INVALID", "FUERA_RANGO", "FRESCA", "0");
    expectSignal(invalidFrame, "RPM", "INVALID", "NO_FINITA", "FRESCA", "0");
    require(brailleGlyphs(invalidFrame) == arc,
            "out-of-range and non-finite readings must not move the needles");
}

void pipeline()
{
    const auto scenario = makeScenario(ScenarioKind::faults);
    require(scenario.has_value(), "the fault scenario must be available");
    Cycle observed = 0;
    const bool finished = runScenario(*scenario, [&](const SignalSnapshot& snapshot, const ControlResult& result) {
        ++observed;
        const auto table = render(snapshot, result, DashboardFormat::ascii);
        const auto dials = render(snapshot, result, DashboardFormat::braille);
        expectText(table, "VEHICLE ECU SIMULATOR | CICLO " + std::to_string(observed) + "\n");
        require(tableAndControl(table) == tableAndControl(dials),
                "both Dashboard formats must show the same data at every cycle");

        if (observed == 1) {
            expectText(table, "Control: SELF_TEST");
            expectText(table, "Accion: INHIBIR");
        } else if (observed == 5) {
            expectSignal(table, "Bateria", "VALID", "VALIDA", "RETENIDA", "1");
            expectText(table, "Control: DEGRADED");
            expectText(table, "Accion: LIMITADA");
            expectText(table, "[DEGRADANTE] SENAL_RETENIDA [Bateria]");
        } else if (observed == 6) {
            expectText(table, "RECUPERACION_PENDIENTE");
        } else if (observed == 9 || observed == 10) {
            expectSignal(table, "Acelerador", "INVALID", "FUERA_RANGO", "FRESCA", "0");
            expectText(table, "MEDICION_FUERA_RANGO [Acelerador]");
        } else if (observed >= 14 && observed <= 17) {
            const auto age = std::to_string(observed - 13);
            for (const auto signal : signalNames) {
                expectSignal(table, signal, observed == 17 ? "MISSING" : "VALID", "VALIDA",
                             observed == 17 ? "CADUCADA" : "RETENIDA", age);
            }
            if (observed == 17) {
                expectText(table, "[CRITICO] SENAL_CADUCADA [Temperatura]");
            }
        } else if (observed == 18) {
            for (const auto signal : signalNames) {
                expectSignal(table, signal, "VALID", "VALIDA", "FRESCA", "0");
            }
            expectText(table, "Diagnostico actual: NINGUNO");
        } else if (observed == 20) {
            expectSignal(table, "Temperatura", "VALID", "VALIDA", "FRESCA", "0");
            expectText(table, "[CRITICO] SOBRE_TEMPERATURA [Temperatura]");
        }

        if (observed >= 17) {
            expectText(table, observed == scenario->cycles() ? "Control: SHUTDOWN" : "Control: SAFE_STATE");
            expectText(table, "Accion: INHIBIR");
            expectText(table, "Causa enclavada: SENAL_CADUCADA [Temperatura]");
            expectText(table, "Ciclo de enclavamiento: 17");
        } else {
            expectText(table, "Causa enclavada: NINGUNA");
        }
    });
    require(finished && observed == scenario->cycles() && observed == 42,
            "the Dashboard must receive every cycle of the real fault scenario");
}

void lifecycle()
{
    Gateway gateway;
    ControlECU control;
    ControlResult initial;
    initial.state = control.state();
    for (const auto format : {DashboardFormat::ascii, DashboardFormat::braille}) {
        const auto frame = render(gateway.snapshot(), initial, format);
        expectText(frame, "Control: INIT");
        expectText(frame, "Accion: INHIBIR");
    }

    auto healthy = auxiliaryReadings();
    healthy.add(SignalId::speed, 40.0);
    healthy.add(SignalId::engine_rpm, 1800.0);
    const auto selfTest = process(gateway, control, 1, healthy);
    for (const auto format : {DashboardFormat::ascii, DashboardFormat::braille}) {
        const auto frame = render(selfTest.snapshot, selfTest.result, format);
        expectText(frame, "Control: SELF_TEST");
        expectText(frame, "Accion: INHIBIR");
        expectText(frame, "Diagnostico actual: NINGUNO");
    }
    const auto running = process(gateway, control, 2, healthy);
    expectText(render(running.snapshot, running.result, DashboardFormat::ascii), "Control: OPERATIONAL");

    SignalBatch critical;
    critical.add(SignalId::coolant_temperature, 151.0);
    const auto fault = process(gateway, control, 3, critical);
    require(fault.result.latchedFault && fault.result.latchedFault->cycle == 3,
            "the shutdown presentation needs an actual latched fault");
    const auto shutdown = process(gateway, control, 4, healthy, ControlEvent::shutdown);
    for (const auto format : {DashboardFormat::ascii, DashboardFormat::braille}) {
        const auto frame = render(shutdown.snapshot, shutdown.result, format);
        expectText(frame, "Control: SHUTDOWN");
        expectText(frame, "Accion: INHIBIR");
        expectText(frame, "Diagnostico actual: NINGUNO");
        expectText(frame, "Causa enclavada: MEDICION_FUERA_RANGO [Temperatura]");
        expectText(frame, "Ciclo de enclavamiento: 3");
        expectSignal(frame, "Temperatura", "VALID", "VALIDA", "FRESCA", "0");
    }
    require(shutdown.result.latchedFault && shutdown.result.latchedFault->cycle == 3 &&
                shutdown.result.state == ControlState::shutdown,
            "rendering must leave shutdown and its original fault unchanged");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

constexpr std::array cases{
    TestCase{"ascii", ascii},
    TestCase{"braille", braille},
    TestCase{"pipeline", pipeline},
    TestCase{"lifecycle", lifecycle},
};

}  // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Expected one test case name.\n";
        return EXIT_FAILURE;
    }

    for (const auto& test : cases) {
        if (test.name == argv[1]) {
            try {
                test.run();
                return EXIT_SUCCESS;
            } catch (const std::exception& error) {
                std::cerr << test.name << ": " << error.what() << '\n';
                return EXIT_FAILURE;
            }
        }
    }
    std::cerr << "Unknown test case: " << argv[1] << '\n';
    return EXIT_FAILURE;
}
