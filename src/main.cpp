#include "ecu/binary.hpp"
#include "ecu/csv.hpp"
#include "ecu/dashboard.hpp"
#include "ecu/simulation.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <clocale>
#include <langinfo.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

constexpr std::string_view usage =
    "Uso: vehicle-ecu-simulator [nominal|faults|battery-fault|oil-fault|temperature-dropout] "
    "[--dashboard auto|live|ascii|braille] [--csv ARCHIVO | --binary ARCHIVO]\n";

enum class LogFormat { csv, binary };
enum class DashboardMode { automatic, live, ascii, braille };

struct Options {
    ecu::ScenarioKind scenario{ecu::ScenarioKind::nominal};
    DashboardMode dashboard{DashboardMode::automatic};
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
                options.dashboard = DashboardMode::ascii;
            } else if (format == "braille") {
                options.dashboard = DashboardMode::braille;
            } else if (format == "live") {
                options.dashboard = DashboardMode::live;
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

class TerminalOutput {
public:
    TerminalOutput()
    {
#if defined(_WIN32)
        CONSOLE_SCREEN_BUFFER_INFO size{};
        if (::GetConsoleScreenBufferInfo(handle_, &size)) {
            columns = static_cast<std::size_t>(size.srWindow.Right - size.srWindow.Left + 1);
            rows = static_cast<std::size_t>(size.srWindow.Bottom - size.srWindow.Top + 1);
        }
        utf8 = ::GetConsoleOutputCP() == CP_UTF8;
#elif defined(__unix__) || defined(__APPLE__)
        if (::isatty(STDOUT_FILENO) == 0) {
            return;
        }

        winsize size{};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
            columns = size.ws_col;
            rows = size.ws_row;
        }

        const char* term = std::getenv("TERM");
        ansi_ = term != nullptr && *term != '\0' && std::string_view(term) != "dumb";
        if ((term == nullptr || std::string_view(term) != "dumb") && std::setlocale(LC_CTYPE, "") != nullptr) {
            const std::string_view encoding = ::nl_langinfo(CODESET);
            utf8 = encoding == "UTF-8" || encoding == "UTF8" || encoding == "utf-8" || encoding == "utf8";
        }
#endif
    }

    TerminalOutput(const TerminalOutput&) = delete;
    TerminalOutput& operator=(const TerminalOutput&) = delete;

    ~TerminalOutput()
    {
#if defined(_WIN32)
        if (liveEnabled_) {
            ::SetConsoleOutputCP(previousCodePage_);
            ::SetConsoleMode(handle_, previousMode_);
        }
#endif
    }

    [[nodiscard]] bool enableLive()
    {
        if (columns < ecu::dashboardMinBrailleColumns || rows < ecu::dashboardMinLiveRows) {
            return false;
        }
#if defined(_WIN32)
        previousCodePage_ = ::GetConsoleOutputCP();
        if (previousCodePage_ == 0 || !::GetConsoleMode(handle_, &previousMode_) ||
            !::SetConsoleMode(handle_, previousMode_ | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            return false;
        }
        if (!::SetConsoleOutputCP(CP_UTF8)) {
            ::SetConsoleMode(handle_, previousMode_);
            return false;
        }
        liveEnabled_ = true;
        return true;
#else
        return ansi_ && utf8;
#endif
    }

    bool utf8{false};
    std::size_t columns{0};

private:
    std::size_t rows{0};
#if defined(_WIN32)
    HANDLE handle_{::GetStdHandle(STD_OUTPUT_HANDLE)};
    DWORD previousMode_{0};
    UINT previousCodePage_{0};
    bool liveEnabled_{false};
#else
    bool ansi_{false};
#endif
};

class LiveDashboard {
public:
    [[nodiscard]] bool render(const ecu::SignalSnapshot& snapshot, const ecu::ControlResult& control)
    {
        std::ostringstream frame;
        if (!ecu::renderLiveDashboard(frame, snapshot, control)) {
            return false;
        }
        constexpr auto period = std::chrono::milliseconds{500};
        const bool firstFrame = !nextFrame_;
        if (firstFrame) {
            nextFrame_ = Clock::now();
        } else {
            std::this_thread::sleep_until(*nextFrame_);
        }
        const auto now = Clock::now();
        if (now >= *nextFrame_ + period) {
            nextFrame_ = now;
        }
        *nextFrame_ += period;

        // La fila final deja el cursor debajo del panel dentro de una terminal de 24 filas.
        if (firstFrame) {
            std::cout << "\033[2J";
        }
        return static_cast<bool>(std::cout << "\033[H" << frame.str() << "\033[J" << std::flush);
    }

private:
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> nextFrame_;
};

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

    const auto scenario = ecu::makeScenario(options->scenario);
    if (!scenario) {
        std::cerr << "No se pudo preparar el escenario.\n";
        return EXIT_FAILURE;
    }

    TerminalOutput terminal;
    const auto mode = options->dashboard;
    const bool live = (mode == DashboardMode::automatic || mode == DashboardMode::live) && terminal.enableLive();
    if (mode == DashboardMode::live && !live) {
        std::cerr << "El Dashboard en vivo requiere una terminal ANSI y UTF-8 de al menos 80 x 24.\n";
        return EXIT_FAILURE;
    }
    const auto format = mode == DashboardMode::braille ? ecu::DashboardFormat::braille
        : mode == DashboardMode::ascii ? ecu::DashboardFormat::ascii
        : ecu::selectDashboardFormat(terminal.utf8, terminal.columns);
    LiveDashboard dashboard;
    bool outputOk = true;
    const auto render = [&](const ecu::SignalSnapshot& snapshot, const ecu::ControlResult& control) {
        if (outputOk) {
            outputOk = live ? dashboard.render(snapshot, control)
                : ecu::renderDashboard(std::cout, snapshot, control, format, terminal.columns);
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
