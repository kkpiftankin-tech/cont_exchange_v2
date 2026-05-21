#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "cex/common/kafka.hpp"
#include "domain/venue_adapter.hpp"

namespace cex::venues::infra {

struct VenueObservabilityTopics {
  std::string venue_health{"venue.health"};
  std::string logs{"logs"};
  std::string metrics{"metrics"};
};

class VenueObservabilityProducer {
 public:
  using ProduceFn = std::function<bool(const std::string& topic,
                                       const std::string& key,
                                       const std::string& payload)>;

  explicit VenueObservabilityProducer(cex::common::KafkaProducer producer,
                                      VenueObservabilityTopics topics = {});

  explicit VenueObservabilityProducer(ProduceFn produce_fn,
                                      VenueObservabilityTopics topics = {});

  bool PublishStatus(const domain::VenueHeartbeat& heartbeat,
                     const std::string& reason,
                     const std::string& routing_mode_override = {});

  bool PublishTraffic(const std::string& venue_id,
                      const std::string& channel,
                      std::size_t payload_bytes,
                      bool ok,
                      const std::string& direction,
                      const std::string& detail = {});

  bool PublishError(const std::string& venue_id,
                    const std::string& message,
                    const std::map<std::string, std::string>& fields = {});

  bool PublishMetric(const std::string& venue_id,
                     const std::string& name,
                     double value,
                     const std::map<std::string, std::string>& labels = {});

 private:
  bool Produce(const std::string& topic,
               const std::string& key,
               const std::string& payload);

  std::unique_ptr<cex::common::KafkaProducer> producer_;
  ProduceFn produce_fn_;
  VenueObservabilityTopics topics_;
};

}  // namespace cex::venues::infra
