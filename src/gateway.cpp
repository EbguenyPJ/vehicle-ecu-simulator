#include "ecu/gateway.hpp"

#include <cmath>

namespace ecu {
namespace {

SampleQuality classify(double value, const SignalDescriptor& signal) noexcept
{
    if (!std::isfinite(value)) {
        return SampleQuality::non_finite;
    }
    if (value < signal.minimum || value > signal.maximum) {
        return SampleQuality::out_of_range;
    }
    return SampleQuality::in_range;
}

}  // namespace

bool InputDiagnostics::hasUnknownId() const noexcept
{
    return unknownId_;
}

bool InputDiagnostics::hasDuplicate(SignalId id) const noexcept
{
    const auto index = detail::signalIndex(id);
    return index && duplicates_[*index];
}

InsertResult SignalBatch::add(SignalId id, double value) noexcept
{
    const auto index = detail::signalIndex(id);
    if (!index) {
        diagnostics_.unknownId_ = true;
        return InsertResult::unknown_id;
    }

    auto& reading = readings_[*index];
    auto& duplicate = diagnostics_.duplicates_[*index];
    if (duplicate || reading.has_value()) {
        duplicate = true;
        reading.reset();
        return InsertResult::duplicate_id;
    }

    reading = value;
    return InsertResult::inserted;
}

Cycle SignalSnapshot::cycle() const noexcept
{
    return cycle_;
}

InputDiagnostics SignalSnapshot::diagnostics() const noexcept
{
    return diagnostics_;
}

GatewayConfig SignalSnapshot::config() const noexcept
{
    return config_;
}

std::optional<SignalObservation> SignalSnapshot::observe(SignalId id) const noexcept
{
    const auto index = detail::signalIndex(id);
    if (!index) {
        return std::nullopt;
    }

    const auto& sample = samples_[*index];
    if (!sample) {
        return SignalObservation{};
    }

    const Cycle age = cycle_ - sample->receivedAt;
    const auto freshness = age == 0 ? Freshness::fresh
        : age <= config_.maxAgeCycles ? Freshness::retained : Freshness::expired;
    const auto status = freshness == Freshness::expired ? SignalStatus::missing
        : sample->quality == SampleQuality::in_range ? SignalStatus::valid : SignalStatus::invalid;
    return SignalObservation{sample, age, freshness, status};
}

Gateway::Gateway(GatewayConfig config) noexcept
{
    current_.config_ = config;
}

SignalSnapshot Gateway::snapshot() const noexcept
{
    return current_;
}

std::optional<SignalSnapshot> Gateway::processCycle(Cycle cycle, const SignalBatch& batch) noexcept
{
    if (cycle <= current_.cycle_) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index < signalCount; ++index) {
        if (const auto& reading = batch.readings_[index]) {
            current_.samples_[index] = SignalSample{
                *reading, classify(*reading, signalCatalog[index]), cycle};
        }
    }

    current_.cycle_ = cycle;
    current_.diagnostics_ = batch.diagnostics_;
    return current_;
}

}  // namespace ecu
