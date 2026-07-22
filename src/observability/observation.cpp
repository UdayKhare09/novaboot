#include "novaboot/observability/observation.h"

#include <format>

namespace novaboot::observability {

std::string MeterRegistry::key_for(std::string_view name, InstrumentKind kind,
                                   std::string_view unit,
                                   const Attributes& attributes) {
    std::string key = std::format("{}|{}|{}", name, static_cast<int>(kind), unit);
    for (const auto& [attribute_name, attribute_value] : attributes) {
        key += std::format("|{}={}", attribute_name, attribute_value);
    }
    return key;
}

std::vector<MetricPoint::HistogramBucket>
MeterRegistry::default_histogram_buckets(std::string_view unit) {
    const std::vector<double> bounds = unit == "s"
        ? std::vector<double>{0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25,
                              0.5, 1.0, 2.5, 5.0, 10.0}
        : std::vector<double>{128, 512, 1024, 4096, 16384, 65536, 262144,
                              1048576, 4194304, 16777216};
    std::vector<MetricPoint::HistogramBucket> buckets;
    buckets.reserve(bounds.size());
    for (const auto bound : bounds) buckets.push_back({bound, 0});
    return buckets;
}

void MeterRegistry::counter_add(std::string_view name, double value,
                                Attributes attributes) {
    std::lock_guard lock(mutex_);
    const auto key = key_for(name, InstrumentKind::Counter, "1", attributes);
    auto& stored = points_[key].point;
    if (stored.name.empty()) {
        stored = MetricPoint{
            .name = std::string(name), .unit = "1", .kind = InstrumentKind::Counter,
            .attributes = std::move(attributes), .buckets = {}};
    }
    stored.value += value;
}

void MeterRegistry::up_down_counter_add(std::string_view name, double value,
                                        Attributes attributes) {
    std::lock_guard lock(mutex_);
    const auto key = key_for(name, InstrumentKind::UpDownCounter, "1", attributes);
    auto& stored = points_[key].point;
    if (stored.name.empty()) {
        stored = MetricPoint{
            .name = std::string(name), .unit = "1", .kind = InstrumentKind::UpDownCounter,
            .attributes = std::move(attributes), .buckets = {}};
    }
    stored.value += value;
}

void MeterRegistry::histogram_record(std::string_view name, double value,
                                     std::string_view unit, Attributes attributes) {
    std::lock_guard lock(mutex_);
    const auto key = key_for(name, InstrumentKind::Histogram, unit, attributes);
    auto& stored = points_[key].point;
    if (stored.name.empty()) {
        stored = MetricPoint{
            .name = std::string(name), .unit = std::string(unit),
            .kind = InstrumentKind::Histogram, .attributes = std::move(attributes), .buckets = {}};
        stored.buckets = default_histogram_buckets(unit);
    }
    ++stored.count;
    stored.sum += value;
    for (auto& bucket : stored.buckets) {
        if (value <= bucket.upper_bound) ++bucket.count;
    }
}

void MeterRegistry::gauge_set(std::string_view name, double value,
                              std::string_view unit, Attributes attributes) {
    std::lock_guard lock(mutex_);
    const auto key = key_for(name, InstrumentKind::Gauge, unit, attributes);
    auto& stored = points_[key].point;
    if (stored.name.empty()) {
        stored = MetricPoint{
            .name = std::string(name), .unit = std::string(unit),
            .kind = InstrumentKind::Gauge, .attributes = std::move(attributes), .buckets = {}};
    }
    stored.value = value;
}

std::vector<MetricPoint> MeterRegistry::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<MetricPoint> result;
    result.reserve(points_.size());
    for (const auto& [_, stored] : points_) result.push_back(stored.point);
    return result;
}

void MeterRegistry::record_span(SpanRecord span) {
    std::lock_guard lock(mutex_);
    if (spans_.size() == max_completed_records_) spans_.erase(spans_.begin());
    spans_.push_back(std::move(span));
}

void MeterRegistry::record_event(EventRecord event) {
    std::lock_guard lock(mutex_);
    if (events_.size() == max_completed_records_) events_.erase(events_.begin());
    events_.push_back(std::move(event));
}

std::vector<SpanRecord> MeterRegistry::spans() const {
    std::lock_guard lock(mutex_);
    return spans_;
}

std::vector<EventRecord> MeterRegistry::events() const {
    std::lock_guard lock(mutex_);
    return events_;
}

} // namespace novaboot::observability
