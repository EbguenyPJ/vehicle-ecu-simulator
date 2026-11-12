#include "ecu/binary.hpp"
#include "ecu/csv.hpp"
#include "ecu/dashboard.hpp"
#include "ecu/simulation.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
#include <clocale>
#include <langinfo.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

constexpr std::string_view usage =
    "Uso: vehicle-ecu-simulator [nominal|faults|battery-fault|oil-fault|temperature-dropout] "
    "[--dashboard auto|ascii|braille] [--csv ARCHIVO | --binary ARCHIVO]\n";

enum class LogFormat { csv, binary };

struct Options {
    ecu::ScenarioKind scenario{ecu::ScenarioKind::nominal};
    std::optional<ecu::DashboardFormat> dashboard;
    const char* logPath{nullptr};
    LogFormat logFormat{LogFormat::csv};
};

std::optional<Options> parseOptions(int argc, char* argv[])
{
    Options options;
    bool scenarioSeen = false;
    bool dashboardSeen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--dashboard") {
            if (dashboardSeen || index + 1 == argc) {
                return std::nullopt;
            }
            dashboardSeen = true;
            const std::string_view format = argv[++index];
            if (format == "ascii") {
                options.dashboard = ecu::DashboardFormat::ascii;
            } else if (format == "braille") {
                options.dashboard = ecu::DashboardFormat::braille;
            } else if (format != "auto") {
                return std::nullopt;
            }
        } else if (arg == "--csv" || arg == "--binary") {
            if (options.logPath || index + 1 == argc) {
                return std::nullopt;
            }
            const std::string_view path = argv[++index];
            if (path.empty() || path.substr(0, 2) == "--") {
                return std::nullopt;
            }
            options.logPath = argv[index];
            options.logFormat = arg == "--csv" ? LogFormat::csv : LogFormat::binary;
        } else if (!scenarioSeen) {
            if (arg == "nominal") {
                options.scenario = ecu::ScenarioKind::nominal;
            } else if (arg == "faults") {
                options.scenario = ecu::ScenarioKind::faults;
            } else if (arg == "battery-fault") {
                options.scenario = ecu::ScenarioKind::battery_fault;
            } else if (arg == "oil-fault") {
                options.scenario = ecu::ScenarioKind::oil_fault;
            } else if (arg == "temperature-dropout") {
                options.scenario = ecu::ScenarioKind::temperature_dropout;
            } else {
                return std::nullopt;
            }
            scenarioSeen = true;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

struct TerminalOutput {
    bool utf8{false};
    std::size_t columns{0};
};

TerminalOutput detectTerminal()
{
    TerminalOutput terminal;
#if defined(__unix__) || defined(__APPLE__)
    if (::isatty(STDOUT_FILENO) == 0) {
        return terminal;
    }

    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
        terminal.columns = size.ws_col;
    }

    const char* term = std::getenv("TERM");
    if ((term == nullptr || std::string_view(term) != "dumb") &&
        std::setlocale(LC_CTYPE, "") != nullptr) {
        const std::string_view encoding = ::nl_langinfo(CODESET);
        terminal.utf8 = encoding == "UTF-8" || encoding == "UTF8" ||
                        encoding == "utf-8" || encoding == "utf8";
    }
#endif
    return terminal;
}

template <typename Writer, typename Render, typename ErrorMessage>
bool runWithLog(const ecu::Scenario& scenario, const char* path, std::string_view format,
                const Render& render, ErrorMessage describe)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::cerr << "No se pudo abrir el archivo " << format << ' ' << path << ".\n";
        return false;
    }
    Writer writer(file);
    const auto log = ecu::recordObserver(writer);
    bool writingOk = true;
    const auto completed = ecu::runScenario(scenario, [&](const ecu::SignalSnapshot& snapshot,
                                                          const ecu::ControlResult& control) {
        writingOk = log(snapshot, control);
        if (writingOk) {
            render(snapshot, control);
        }
        return writingOk;
    });
    const bool flushed = writer.finish();
    file.close();
    if (!writingOk || !flushed || !file) {
        auto error = writer.error();
        using Error = decltype(error);
        if (error == Error::none) {
            error = writingOk ? Error::io_error : Error::invalid_record;
        }
        std::cerr << "No se pudo guardar el archivo " << format << " (" << describe(error) << ").\n";
        return false;
    }
    if (!completed) {
        std::cerr << "No se pudo completar el escenario.\n";
    }
    return completed;
}

}  // namespace

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << usage;
        return EXIT_SUCCESS;
    }

    const auto options = parseOptions(argc, argv);
    if (!options) {
        std::cerr << usage;
        return EXIT_FAILURE;
    }

    const auto terminal = detectTerminal();
    const auto format = options->dashboard.value_or(ecu::selectDashboardFormat(terminal.utf8, terminal.columns));
    const auto scenario = ecu::makeScenario(options->scenario);
    if (!scenario) {
        std::cerr << "No se pudo preparar el escenario.\n";
        return EXIT_FAILURE;
    }

    bool outputOk = true;
    const auto render = [&](const ecu::SignalSnapshot& snapshot, const ecu::ControlResult& control) {
        if (outputOk) {
            outputOk = ecu::renderDashboard(std::cout, snapshot, control, format, terminal.columns);
        }
    };

    if (options->logPath) {
        const bool completed = options->logFormat == LogFormat::csv
            ? runWithLog<ecu::CsvWriter>(*scenario, options->logPath, "CSV", render, ecu::csvErrorMessage)
            : runWithLog<ecu::BinaryWriter>(*scenario, options->logPath, "binario", render, ecu::binaryErrorMessage);
        if (!completed) {
            return EXIT_FAILURE;
        }
    } else if (!ecu::runScenario(*scenario, render)) {
        std::cerr << "No se pudo completar el escenario.\n";
        return EXIT_FAILURE;
    }
    if (!outputOk || !std::cout.flush()) {
        std::cerr << "No se pudo escribir el dashboard.\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
