#include "client/output_formatter_impl.h"

#include <google/protobuf/util/time_util.h>

#include <chrono>
#include <sstream>

#include "nighthawk/common/exception.h"

#include "external/envoy/source/common/protobuf/utility.h"

#include "api/client/fortio.pb.h"

#include "absl/strings/str_cat.h"

namespace Nighthawk {
namespace Client {

std::vector<std::string> OutputFormatterImpl::getLowerCaseOutputFormats() {
  const Envoy::Protobuf::EnumDescriptor* enum_descriptor =
      nighthawk::client::OutputFormat::OutputFormatOptions_descriptor();
  std::vector<std::string> values;
  // We skip the first, which is DEFAULT, as it's not selectable.
  for (int i = 1; i < enum_descriptor->value_count(); ++i) {
    auto* value_descriptor = enum_descriptor->value(i);
    std::string name = value_descriptor->name();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    values.push_back(name);
  }
  return values;
}

void OutputFormatterImpl::iteratePercentiles(
    const nighthawk::client::Statistic& statistic,
    const std::function<void(const nighthawk::client::Percentile&)>& callback) const {
  // The proto percentiles are ordered ascending. We write the first match to the stream.
  double last_percentile = -1.;
  for (const double p : {.0, .5, .75, .8, .9, .95, .99, .999, 1.}) {
    for (const auto& percentile : statistic.percentiles()) {
      if (percentile.percentile() >= p && last_percentile < percentile.percentile()) {
        last_percentile = percentile.percentile();
        callback(percentile);
        break;
      }
    }
  }
}

std::string ConsoleOutputFormatterImpl::formatProto(const nighthawk::client::Output& output) const {
  std::stringstream ss;
  ss << "Nighthawk - A layer 7 protocol benchmarking tool." << std::endl << std::endl;
  for (const auto& result : output.results()) {
    if (result.name() == "global") {
      for (const auto& statistic : result.statistics()) {
        if (statistic.count() == 0) {
          continue;
        }
        ss << fmt::format("{}", statIdtoFriendlyStatName(statistic.id())) << std::endl;
        ss << fmt::format("  samples: {}", statistic.count()) << std::endl;
        ss << fmt::format("  mean:    {}", formatProtoDuration(statistic.mean())) << std::endl;
        ss << fmt::format("  pstdev:  {}", formatProtoDuration(statistic.pstdev())) << std::endl;
        ss << std::endl;
        ss << fmt::format("  {:<{}}{:<{}}{:<{}}", "Percentile", 12, "Count", 12, "Latency", 15)
           << std::endl;
        iteratePercentiles(statistic, [&ss, this](const nighthawk::client::Percentile& percentile) {
          ss << fmt::format("  {:<{}}{:<{}}{:<{}}", percentile.percentile(), 12, percentile.count(),
                            12, formatProtoDuration(percentile.duration()), 15)
             << std::endl;
        });
        ss << std::endl;
      }
      ss << fmt::format("{:<{}}{:<{}}{}", "Counter", 40, "Value", 12, "Per second") << std::endl;
      for (const auto& counter : result.counters()) {
        ss << fmt::format("{:<{}}{:<{}}{:.{}f}", counter.name(), 40, counter.value(), 12,
                          counter.value() / (output.options().duration().seconds() * 1.0), 2)
           << std::endl;
      }
      ss << std::endl;
    }
  }

  return ss.str();
}

std::string ConsoleOutputFormatterImpl::formatProtoDuration(
    const Envoy::ProtobufWkt::Duration& duration) const {
  auto c = Envoy::Protobuf::util::TimeUtil::DurationToMicroseconds(duration);
  return fmt::format("{}s {:03}ms {:03}us", (c % 1'000'000'000) / 1'000'000,
                     (c % 1'000'000) / 1'000, c % 1'000);
}

std::string ConsoleOutputFormatterImpl::statIdtoFriendlyStatName(absl::string_view stat_id) {
  if (stat_id == "benchmark_http_client.queue_to_connect") {
    return "Queueing and connection setup latency";
  } else if (stat_id == "benchmark_http_client.request_to_response") {
    return "Request start to response end";
  } else if (stat_id == "sequencer.callback") {
    return "Initiation to completion";
  } else if (stat_id == "sequencer.blocking") {
    return "Blocking. Results are skewed when significant numbers are reported here.";
  }
  return std::string(stat_id);
}

std::string JsonOutputFormatterImpl::formatProto(const nighthawk::client::Output& output) const {
  return Envoy::MessageUtil::getJsonStringFromMessage(output, true, true);
}

std::string YamlOutputFormatterImpl::formatProto(const nighthawk::client::Output& output) const {
  return Envoy::MessageUtil::getYamlStringFromMessage(output, true, true);
}

std::string
DottedStringOutputFormatterImpl::formatProto(const nighthawk::client::Output& output) const {
  std::stringstream ss;
  for (const auto& result : output.results()) {
    for (const auto& statistic : result.statistics()) {
      const std::string prefix = fmt::format("{}.{}", result.name(), statistic.id());

      ss << fmt::format("{}.samples: {}", prefix, statistic.count()) << std::endl;
      ss << fmt::format("{}.mean: {}", prefix,
                        Envoy::Protobuf::util::TimeUtil::DurationToMicroseconds(statistic.mean()))
         << std::endl;
      ss << fmt::format("{}.pstdev: {}", prefix,
                        Envoy::Protobuf::util::TimeUtil::DurationToMicroseconds(statistic.pstdev()))
         << std::endl;

      iteratePercentiles(statistic, [&ss, prefix](const nighthawk::client::Percentile& percentile) {
        const std::string percentile_prefix =
            fmt::format("{}.permilles-{:.{}f}", prefix, percentile.percentile() * 1000, 0);
        ss << fmt::format("{}.count: {}", percentile_prefix, percentile.count()) << std::endl;
        ss << fmt::format(
                  "{}.microseconds: {}", percentile_prefix,
                  Envoy::Protobuf::util::TimeUtil::DurationToMicroseconds(percentile.duration()))
           << std::endl;
      });
    }
    for (const auto& counter : result.counters()) {
      const std::string prefix = fmt::format("{}.{}", result.name(), counter.name());
      ss << fmt::format("{}:{}", prefix, counter.value()) << std::endl;
    }
  }
  return ss.str();
}

const nighthawk::client::Result&
FortioOutputFormatterImpl::getGlobalResult(const nighthawk::client::Output& output) const {
  for (const auto& nh_result : output.results()) {
    if (nh_result.name() == "global") {
      return nh_result;
    }
  }

  throw NighthawkException("Nighthawk output was malformed, contains no 'global' results.");
}

const nighthawk::client::Counter&
FortioOutputFormatterImpl::getCounterByName(const nighthawk::client::Result& result,
                                            absl::string_view counter_name) const {
  for (const auto& nh_counter : result.counters()) {
    if (nh_counter.name() == counter_name) {
      return nh_counter;
    }
  }

  throw NighthawkException(absl::StrCat(
      "Nighthawk result was malformed, contains no counter with name: ", counter_name));
}

const nighthawk::client::Statistic& FortioOutputFormatterImpl::getRequestResponseStatistic(
    const nighthawk::client::Result& result) const {
  for (auto const& nh_stat : result.statistics()) {
    if (nh_stat.id() == "benchmark_http_client.request_to_response") {
      return nh_stat;
    }
  }

  throw NighthawkException("Nighthawk result was malformed, contains no "
                           "'benchmark_http_client.request_to_response' statistic.");
}

std::string FortioOutputFormatterImpl::formatProto(const nighthawk::client::Output& output) const {
  nighthawk::client::FortioResult fortio_output;

  // TODO(nareddyt): Not needed but nice to have, displays in the UI
  fortio_output.mutable_labels()->set_value("A random label");

  fortio_output.mutable_starttime()->set_seconds(output.timestamp().seconds());
  fortio_output.mutable_requestedqps()->set_value(output.options().requests_per_second().value());
  fortio_output.mutable_requestedduration()->set_seconds(output.options().duration().seconds());

  // FIXME: Set the actual to the same as the requested since NH does not expose these fields
  fortio_output.mutable_actualqps()->set_value(output.options().requests_per_second().value());
  fortio_output.mutable_actualduration()->set_value(output.options().duration().seconds());

  // This displays as "connections" in the UI, not threads
  fortio_output.mutable_numthreads()->set_value(output.options().connections().value());

  // This displays the endpoint URL in the UI
  fortio_output.mutable_url()->set_value(output.options().uri().value());

  // Get the result that represents all workers (global)
  const auto& nh_global_result = this->getGlobalResult(output);

  // Fill in the number of successful responses.
  // Fortio-ui only reads the 200 OK field, other fields are only informational.
  const auto& nh_2xx_counter = this->getCounterByName(nh_global_result, "benchmark.http_2xx");
  fortio_output.mutable_retcodes()->insert({"200", 0});
  fortio_output.mutable_retcodes()->at("200") = nh_2xx_counter.value();

  // The core of the transformation is here: All the percentiles to display the dashboard
  const auto& nh_stat = this->getRequestResponseStatistic(nh_global_result);

  // Set the count
  nighthawk::client::DurationHistogram fortio_histogram;
  fortio_histogram.mutable_count()->set_value(nh_stat.count());

  // Tracking variables
  uint64_t prev_fortio_count = 0;
  double prev_fortio_end = 0;

  for (int i = 0; i < nh_stat.percentiles().size(); i++) {

    const auto& nh_percentile = nh_stat.percentiles().at(i);

    // Create the data entry
    nighthawk::client::DataEntry fortio_data_entry;

    // fortio_percent = 100 * nh_percentile
    fortio_data_entry.mutable_percent()->set_value(nh_percentile.percentile() * 100);

    // fortio_count = nh_count - prev_fortio_count
    fortio_data_entry.mutable_count()->set_value(nh_percentile.count() - prev_fortio_count);

    // fortio_end = nh_duration (need to convert formats)
    const double nh_duration_in_double = nh_percentile.duration().nanos() / 1e9;
    fortio_data_entry.mutable_end()->set_value(nh_duration_in_double);

    // fortio_start = prev_fortio_end
    if (i == 0) {
      // If this is the first entry, force the start and end time to be the same
      prev_fortio_end = nh_duration_in_double;
    }
    fortio_data_entry.mutable_start()->set_value(prev_fortio_end);

    // Update tracking variables
    prev_fortio_count = nh_percentile.count();
    prev_fortio_end = nh_duration_in_double;

    // Set the data entry in the histogram only if it's not the first entry
    fortio_histogram.add_data()->CopyFrom(fortio_data_entry);
  }

  // Set the histogram in main fortio output
  fortio_output.mutable_durationhistogram()->CopyFrom(fortio_histogram);

  return Envoy::MessageUtil::getJsonStringFromMessage(fortio_output, true, true);
}

} // namespace Client
} // namespace Nighthawk