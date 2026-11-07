#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace ecu::detail {

inline constexpr std::size_t brailleDialWidth = 27;
inline constexpr std::size_t brailleDialHeight = 7;

// Fracción de la escala entre 0 y 1. Sin valor se dibuja solo la carátula.
[[nodiscard]] std::array<std::string, brailleDialHeight> drawBrailleDial(std::optional<double> fraction);

}  // namespace ecu::detail
