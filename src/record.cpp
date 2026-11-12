#include "ecu/record.hpp"

#include <algorithm>
#include <cmath>

namespace ecu {
namespace {

bool validDiagnostic(const Diagnostic& diagnostic) noexcept
{
    return diagnostic.code >= DiagnosticCode::signal_missing &&
           diagnostic.code <= DiagnosticCode::recovery_pending &&
           (diagnostic.severity == Severity::degraded || diagnostic.severity == Severity::critical) &&
           (!diagnostic.signal || detail::signalIndex(*diagnostic.signal).has_value());
}

bool validObservation(const SignalObservation& observation, Cycle cycle, Cycle maxAgeCycles) noexcept
{
    if (observation.sample) {
        const auto& sample = *observation.sample;
        const bool nonFinite = !std::isfinite(sample.value);
        if (sample.receivedAt == 0 || sample.receivedAt > cycle ||
            sample.quality < SampleQuality::in_range || sample.quality > SampleQuality::non_finite ||
            nonFinite != (sample.quality == SampleQuality::non_finite)) {
            return false;
        }
    }
    const auto expected = detail::observeSample(observation.sample, cycle, maxAgeCycles);
    return observation.age == expected.age && observation.freshness == expected.freshness &&
           observation.status == expected.status;
}

}  // namespace

bool isValidRecord(const CycleRecord& record) noexcept
{
    if (record.state < ControlState::init || record.state > ControlState::shutdown ||
        record.action < ControlAction::inhibit_output || record.action > ControlAction::normal_operation ||
        record.diagnosticCount > record.diagnostics.size()) {
        return false;
    }
    if (!std::all_of(record.signals.begin(), record.signals.end(), [&](const auto& observation) {
            return validObservation(observation, record.cycle, record.gatewayConfig.maxAgeCycles);
        })) {
        return false;
    }
    const auto diagnosticEnd = record.diagnostics.data() + record.diagnosticCount;
    if (!std::all_of(record.diagnostics.data(), diagnosticEnd, validDiagnostic)) {
        return false;
    }
    return !record.latchedFault ||
        (record.latchedFault->cycle > 0 && record.latchedFault->cycle <= record.cycle &&
         record.latchedFault->cause.severity == Severity::critical &&
         validDiagnostic(record.latchedFault->cause));
}

std::optional<CycleRecord> makeRecord(const SignalSnapshot& snapshot, const ControlResult& result) noexcept
{
    if (snapshot.cycle() != result.cycle) {
        return std::nullopt;
    }
    CycleRecord record;
    record.cycle = result.cycle;
    record.gatewayConfig = snapshot.config();
    record.state = result.state;
    record.action = result.action;
    const auto input = snapshot.diagnostics();
    record.unknownId = input.hasUnknownId();
    for (std::size_t index = 0; index < signalCount; ++index) {
        const auto id = signalCatalog[index].id;
        record.signals[index] = *snapshot.observe(id);
        record.duplicates[index] = input.hasDuplicate(id);
    }
    record.diagnosticCount = result.diagnostics.size();
    std::copy(result.diagnostics.begin(), result.diagnostics.end(), record.diagnostics.begin());
    record.latchedFault = result.latchedFault;
    if (!isValidRecord(record)) {
        return std::nullopt;
    }
    return record;
}

}  // namespace ecu
