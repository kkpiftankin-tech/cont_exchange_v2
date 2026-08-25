#include "hedge_pnl_aggregator.hpp"
#include <unordered_map>

namespace cex::market_data::app {

struct HedgePnlKey {
    std::string batch_id;
    std::string session_id;
    std::string user_id;
    std::string venue;
    std::string instrument;
    
    bool operator==(const HedgePnlKey& other) const {
        return batch_id == other.batch_id &&
               session_id == other.session_id &&
               user_id == other.user_id &&
               venue == other.venue &&
               instrument == other.instrument;
    }
};

struct HedgePnlKeyHash {
    size_t operator()(const HedgePnlKey& k) const {
        return std::hash<std::string>()(k.batch_id) ^
               (std::hash<std::string>()(k.session_id) << 1) ^
               (std::hash<std::string>()(k.user_id) << 2) ^
               (std::hash<std::string>()(k.venue) << 3) ^
               (std::hash<std::string>()(k.instrument) << 4);
    }
};

class HedgePnLAggregator {
public:
    void ProcessExecutionReport(const fob::execution::v1::ExecutionReport& report) {
        HedgePnlKey key{
            .batch_id = report.batch_id(),
            .session_id = report.meta().session_id(),
            .user_id = report.meta().user_id(),
            .venue = report.venue(),
            .instrument = report.instrument().symbol()
        };
        
        double pnl = decimal_to_double(report.hedge_pnl().units(), 
                                       report.hedge_pnl().scale());
        double qty = decimal_to_double(report.filled_qty().units(),
                                       report.filled_qty().scale());
        
        auto& agg = aggregates_[key];
        agg.total_pnl += pnl;
        agg.total_qty += qty;
        agg.trade_count++;
        agg.last_update = std::chrono::system_clock::now();
    }
    
    void FlushToClickHouse() {
        for (const auto& [key, agg] : aggregates_) {
            SaveAggregate(key, agg);
        }
        aggregates_.clear();
    }
    
private:
    struct Aggregate {
        double total_pnl{0};
        double total_qty{0};
        uint64_t trade_count{0};
        std::chrono::system_clock::time_point last_update;
    };
    
    std::unordered_map<HedgePnlKey, Aggregate, HedgePnlKeyHash> aggregates_;
    
    void SaveAggregate(const HedgePnlKey& key, const Aggregate& agg) {
        // INSERT INTO hedge_pnl_agg ...
    }
    
    double decimal_to_double(int64_t units, int32_t scale) {
        return static_cast<double>(units) / std::pow(10.0, scale);
    }
};

}  // namespace