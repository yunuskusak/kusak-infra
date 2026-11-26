#include "vpin_velocity_tracker.hpp"
#include <cstdio>
#include <chrono>
using namespace kusak::research;
static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::printf("FAIL %d: %s\n",__LINE__,#c); ++fails;} else std::printf("ok: %s\n",#c);}while(0)
static constexpr int64_t S=VPIN_SCALE;
int main(){
  VPINConfig c; c.bucket_volume_scaled=100*S; c.min_buckets_valid=2; c.n_buckets_window=50;
  { VPINVelocityTracker t(c);
    t.update_trade(30*S,0,true,0); t.update_trade(250*S,0,false,0);
    CHECK(t.state().bucket_count==2);              // 280 = 2*100 + 80
    CHECK(t.state().window_vol_scaled==200*S);
  }
  { VPINVelocityTracker t(c);                      // tek islem 1050 -> 10 kova + 50 acik
    t.update_trade(1050*S,0,true,0);
    CHECK(t.state().bucket_count==10); CHECK(t.state().window_vol_scaled==1000*S);
    CHECK(t.state().vpin_scaled==S);               // hepsi tek yon
    t.update_trade(50*S,0,false,0);                // acik kova 50 buy + 50 sell = dengeli
    CHECK(t.state().bucket_count==11);
  }
  { VPINVelocityTracker t(c);                      // karisik: toplam hacim korunumu
    int64_t in=0; for(int i=0;i<1000;++i){ int64_t q=(7+i*13%400)*S/3; in+=q; t.update_trade(q,0,i%3!=0,0);} 
    const int64_t kept = (int64_t)t.state().bucket_count*100*S;
    CHECK(in - kept >= 0 && in - kept < 100*S);    // kalan = acik kova < bvol
  }
  { VPINConfig d=c; d.bucket_volume_scaled=1; VPINVelocityTracker t(d);   // dev islem DoS korumasi
    auto t0=std::chrono::steady_clock::now();
    t.update_trade(INT64_C(4000000000000000000),0,true,0);
    auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-t0).count();
    CHECK(us<5000); CHECK(t.state().bucket_count==4000000000000000000ULL);
  }
  { VPINConfig a=c; a.bucket_volume_scaled=0; a.calib_trades=3; a.bucket_size_multiplier_scaled=10*S; VPINVelocityTracker t(a);
    for(int i=0;i<3;++i) t.update_trade(10*S,0,true,0);
    CHECK(t.calibrated()); CHECK(t.state().bucket_count==0); CHECK(t.effective_bucket_volume_scaled()==100*S);  // kalibrasyon islemleri kovaya girmez
    t.update_trade(100*S,0,false,0); CHECK(t.state().bucket_count==1);
  }
  { VPINConfig e=c; e.n_buckets_window=3; VPINVelocityTracker t(e);
    t.update_trade(500*S,0,true,0); CHECK(t.state().window_vol_scaled==300*S); CHECK(t.state().bucket_count==5);
  }
  std::printf("SONUC fails=%d\n",fails); return fails?1:0; }
