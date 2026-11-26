#include "include/regime_detector.hpp"

kusak::research::MarketRegime test_hft_branchless(kusak::research::RegimeDetectorV2& detector, kusak::research::RegimeSnapshot& snap) {
    return detector.update(snap);
}
