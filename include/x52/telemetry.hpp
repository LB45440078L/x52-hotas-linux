// SPDX-License-Identifier: BSD-3-Clause
//
// Game telemetry sources.
//
// A source is polled a few times per second and produces a flat set of named
// values ("ias", "alt", "gear", ...).  Everything downstream - the MFD
// template, the LED rules - only knows those names, so adding DCS (export.lua)
// or Elite (journal files) later is a new source, not a new pipeline.
//
#ifndef X52_TELEMETRY_HPP
#define X52_TELEMETRY_HPP

#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace x52 {

using TelemetryValues = std::map<std::string, double>;

enum class TelemetrySourceKind { None = 0, WarThunder };

const char* telemetry_source_name(TelemetrySourceKind kind) noexcept;
bool telemetry_source_from_name(std::string_view name, TelemetrySourceKind& out) noexcept;

struct TelemetrySample {
    bool valid = false;
    bool connected = false;
    std::string source;
    std::string error;
    TelemetryValues values;
};

// Minimal blocking HTTP/1.1 GET.  Only ever called from the telemetry thread,
// never from the input loop.
bool http_get(const std::string& host, int port, const std::string& path, std::string& body,
              int timeout_ms = 400);

// Pure parsers, exposed for unit tests.
bool parse_warthunder(const std::string& state_json, const std::string& indicators_json,
                      TelemetryValues& out, std::string& error);

// Expands "{ias:04.0f}" / "{alt}" / "{mach:.2f}" against the value map.
// Missing values render as "--".
std::string format_telemetry_line(const std::string& tmpl, const TelemetryValues& values);

class TelemetrySource {
public:
    virtual ~TelemetrySource() = default;
    virtual TelemetrySourceKind kind() const = 0;
    virtual bool poll(TelemetryValues& out, std::string& error) = 0;
};

std::unique_ptr<TelemetrySource> make_telemetry_source(TelemetrySourceKind kind);

}  // namespace x52

#endif  // X52_TELEMETRY_HPP
