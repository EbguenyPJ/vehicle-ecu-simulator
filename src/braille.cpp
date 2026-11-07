#include "braille.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace ecu::detail {
namespace {

using Canvas = std::array<std::uint8_t, brailleDialWidth * brailleDialHeight>;

struct Point {
    int x;
    int y;
};

constexpr int dotsWide = static_cast<int>(brailleDialWidth) * 2;
constexpr int dotsHigh = static_cast<int>(brailleDialHeight) * 4;
constexpr Point dialCenter{dotsWide / 2 - 1, dotsHigh - 7};
constexpr int dialRadius = 21;
constexpr double pi = 3.14159265358979323846;

void setDot(Canvas& canvas, Point point) noexcept
{
    if (point.x < 0 || point.x >= dotsWide || point.y < 0 || point.y >= dotsHigh) {
        return;
    }

    // Cada carácter braille contiene dos columnas de cuatro puntos.
    constexpr std::uint8_t bits[4][2] = {
        {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80},
    };
    const auto column = static_cast<std::size_t>(point.x / 2);
    const auto row = static_cast<std::size_t>(point.y / 4);
    canvas[row * brailleDialWidth + column] |= bits[point.y % 4][point.x % 2];
}

void drawLine(Canvas& canvas, Point from, Point to) noexcept
{
    const int dx = std::abs(to.x - from.x);
    const int dy = -std::abs(to.y - from.y);
    const int stepX = from.x < to.x ? 1 : -1;
    const int stepY = from.y < to.y ? 1 : -1;
    int error = dx + dy;

    // Bresenham conecta puntos vecinos sin dejar huecos en la aguja.
    for (;;) {
        setDot(canvas, from);
        if (from.x == to.x && from.y == to.y) {
            return;
        }

        const int twiceError = 2 * error;
        if (twiceError >= dy) {
            error += dy;
            from.x += stepX;
        }
        if (twiceError <= dx) {
            error += dx;
            from.y += stepY;
        }
    }
}

Point pointOnDial(double fraction, int radius) noexcept
{
    // Barrido de 210 grados de izquierda a derecha.
    const double radians = (195.0 - 210.0 * fraction) * pi / 180.0;
    return {dialCenter.x + static_cast<int>(std::lround(radius * std::cos(radians))),
            dialCenter.y - static_cast<int>(std::lround(radius * std::sin(radians)))};
}

Canvas makeDialFace() noexcept
{
    Canvas canvas{};
    constexpr int arcSteps = 84;
    Point previous = pointOnDial(0.0, dialRadius);
    for (int step = 1; step <= arcSteps; ++step) {
        const auto fraction = static_cast<double>(step) / arcSteps;
        const Point current = pointOnDial(fraction, dialRadius);
        drawLine(canvas, previous, current);
        previous = current;
    }
    for (int tick = 0; tick <= 4; ++tick) {
        const double fraction = tick / 4.0;
        drawLine(canvas, pointOnDial(fraction, dialRadius - 3), pointOnDial(fraction, dialRadius));
    }
    return canvas;
}

void appendCell(std::string& row, std::uint8_t dots)
{
    if (dots == 0) {
        row.push_back(' ');
        return;
    }

    // Codificación UTF-8 del patrón U+2800 + dots.
    row.push_back(static_cast<char>(0xE2));
    row.push_back(static_cast<char>(0xA0 + (dots >> 6)));
    row.push_back(static_cast<char>(0x80 + (dots & 0x3F)));
}

}  // namespace

std::array<std::string, brailleDialHeight> drawBrailleDial(std::optional<double> fraction)
{
    static const Canvas dialFace = makeDialFace();
    Canvas canvas = dialFace;
    if (fraction && std::isfinite(*fraction)) {
        const double bounded = std::clamp(*fraction, 0.0, 1.0);
        drawLine(canvas, dialCenter, pointOnDial(bounded, dialRadius - 6));
    }

    std::array<std::string, brailleDialHeight> rows;
    for (std::size_t row = 0; row < rows.size(); ++row) {
        rows[row].reserve(brailleDialWidth * 3);
        for (std::size_t column = 0; column < brailleDialWidth; ++column) {
            appendCell(rows[row], canvas[row * brailleDialWidth + column]);
        }
    }
    return rows;
}

}  // namespace ecu::detail
