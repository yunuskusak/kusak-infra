#include "include/regime_detector.hpp"
#include <iostream>
#include <x86intrin.h>

// Private fonksiyonu ölçmek için ufak bir köprü
namespace kusak::research {
    struct TestProxy {
        static MarketRegime measure(const RegimeDetectorV2& d, const RegimeSnapshot& s) {
            return d.classify_raw(s);
        }
    };
}

int main() {
    // RegimeDetectorV2 içinde classify_raw private olduğu için V1 üzerinden ölçelim 
    // (V1'de classify_raw public tanımlanmıştı ve mimari birebir aynı)
    kusak::research::RegimeDetector detector;
    kusak::research::RegimeSnapshot snap;
    
    snap.vpin = 0.8;
    snap.mid_price = 50000.0;
    snap.atr = 5.0;
    snap.volume_1m = 1000.0;

    // Önbelleği ısıt
    for(int i=0; i<10000; ++i) {
        detector.classify_raw(snap);
    }

    unsigned int ui;
    uint64_t start = __rdtscp(&ui);
    
    // 1000 kez çalıştırıp ortalamasını alalım ki daha hassas olsun
    for(int i=0; i<1000; ++i) {
        detector.classify_raw(snap);
    }
    
    uint64_t end = __rdtscp(&ui);

    std::cout << "Classify_raw Ortalama Çevrim Süresi: " << (end - start) / 1000.0 << " cycle" << std::endl;
    return 0;
}
