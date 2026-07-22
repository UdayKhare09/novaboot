#pragma once

#include <cstdint>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace novaboot::observability {

/// Low-cardinality dimensions attached to an observation.
///
/// Attribute names deliberately follow OpenTelemetry semantic conventions.
using Attributes = std::map<std::string, std::string>;

enum class InstrumentKind { Counter, UpDownCounter, Histogram, Gauge };

struct MetricPoint {
    std::string name;
    std::string unit;
    InstrumentKind kind = InstrumentKind::Counter;
    Attributes attributes;
    double value = 0.0;
    std::uint64_t count = 0;
    double sum = 0.0;
    struct HistogramBucket {
        double upper_bound = 0.0;
        std::uint64_t count = 0;
    };
    std::vector<HistogramBucket> buckets;
};

struct SpanRecord {
    std::string name;
    Attributes attributes;
    std::chrono::steady_clock::duration duration{};
    bool error = false;
};

struct EventRecord {
    std::string name;
    Attributes attributes;
};

/// A small, exporter-neutral in-process meter registry.
///
/// It is intentionally independent of an SDK. Applications can read the
/// snapshot directly today and an OTLP adapter can consume the same contract
/// later without exposing a vendor type in application code.
class MeterRegistry {
public:
    void counter_add(std::string_view name, double value = 1.0,
                     Attributes attributes = {});
    void up_down_counter_add(std::string_view name, double value,
                             Attributes attributes = {});
    void histogram_record(std::string_view name, double value,
                          std::string_view unit, Attributes attributes = {});
    void gauge_set(std::string_view name, double value,
                   std::string_view unit, Attributes attributes = {});

    [[nodiscard]] std::vector<MetricPoint> snapshot() const;
    void record_span(SpanRecord span);
    void record_event(EventRecord event);
    [[nodiscard]] std::vector<SpanRecord> spans() const;
    [[nodiscard]] std::vector<EventRecord> events() const;

private:
    struct StoredPoint {
        MetricPoint point;
    };

    static std::string key_for(std::string_view name, InstrumentKind kind,
                               std::string_view unit,
                               const Attributes& attributes);
    static std::vector<MetricPoint::HistogramBucket>
    default_histogram_buckets(std::string_view unit);

    mutable std::mutex mutex_;
    std::map<std::string, StoredPoint> points_;
    std::vector<SpanRecord> spans_;
    std::vector<EventRecord> events_;
    static constexpr std::size_t max_completed_records_ = 1024;
};

} // namespace novaboot::observability
