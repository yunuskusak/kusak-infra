// [P0-02-TEST] KZ-P0-02 kirmizi test. Derleme: g++ -std=c++20 -O2 -Wall -Wextra -Iinclude tools/poll_id_test.cpp -o ~/kusak_build/tools/poll_id_test -lssl -lcrypto -lcurl -lpthread
// Amac: drain_orders_on_shutdown'in urettigi POLL gorevinin order_id'sinin yerel FSM ID'si olmadigini dogrulamak.
// Beklenti: P1/P2 mevcut kodda FAIL (KZ-P0-02 kaniti), P3 sonucu kaydedilir, P4 karakterizasyon (yesil).
// Kapsam siniri: src/main.cpp:1386-1408 icindeki satir ici POLL kopyasi bu testten cagrilamaz, kapsanmaz.
#include <cstdint>
uint64_t g_tsc_ns_mult = (1ULL<<32);
#include "strategy_core.hpp"
#include <cstdio>
#include <thread>
#include <memory>
#include <cstring>
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>
using namespace kusak;
static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n",__LINE__,#c); ++fails;} else std::printf("  ok: %s\n",#c);}while(0)

struct Cap { TaskKind kind; uint64_t order_id; uint64_t local_id; char coid[40]; };

// FlatOrderMap::slots[] varsayilan ilklenmiyor (types.hpp:278); heap yeniden kullaniminda eski slot cop verisi
// kalir ve senaryolari birbirine sizdirir. Test tarafinda her taze context'in slotlari sifirlanir (kaynak degismez).
static std::unique_ptr<StrategyContext> fresh(const config::KusakConfig& c){
    auto p=std::make_unique<StrategyContext>(c);
    std::memset(static_cast<void*>(p->flat_order_map.slots),0,sizeof(p->flat_order_map.slots));
    return p;
}
static OrderResult mk(uint64_t local, OrderStatus st, uint64_t ex, const char* err=nullptr){
    OrderResult r{}; r.local_order_id=local; r.order_id=local; r.exchange_order_id=ex; r.status=st;
    r.is_buy=true; r.ts_recv_ns=tsc_ns();
    if(err) r.error_msg.set(err,(uint8_t)std::strlen(err));
    return r;
}
// Yerel ID slotu: SUBMITTED, active_order_id = fsm_key = L (strategy_core.hpp:1351 mantigi)
static void setup_local(StrategyContext& ctx, uint64_t L){
    ctx.fsm_entry_seq = L;
    ctx.flat_order_map.insert(L,1.0);
    for(auto s:{OrderState::CREATED,OrderState::PREFLIGHT,OrderState::SUBMITTED}) ctx.flat_order_map.transition_key(L,s);
    ctx.fsm_key=L; ctx.active_order_id=L; ctx.awaiting_fill=true; ctx.pending_is_buy=true;
}
// Sahte order-thread: tum gorevleri yakalar, sonuc GONDERMEZ.
static std::vector<Cap> run_drain(StrategyContext& ctx, uint32_t ms, size_t& left, long long& took, int& other){
    std::vector<Cap> caps; std::atomic<bool> run{true}; other=0;
    std::thread fake([&]{ OrderTask t{}; while(run){
        if(ctx.order_channel.pop_task(t)){
            if(t.kind==TaskKind::POLL){ Cap c{}; c.kind=t.kind; c.order_id=t.order_id; c.local_id=t.local_order_id;
                std::snprintf(c.coid,sizeof c.coid,"%s",t.client_oid.c_str()); caps.push_back(c); }
            else ++other;
        } else std::this_thread::sleep_for(std::chrono::microseconds(200)); } });
    auto t0=std::chrono::steady_clock::now();
    left=drain_orders_on_shutdown(ctx,ms);
    took=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
    run=false; fake.join();
    return caps;
}
static void print_caps(const std::vector<Cap>& caps,int other,size_t left,long long ms){
    std::printf("  drain: left=%zu sure=%lldms POLL=%zu diger_gorev=%d\n",left,ms,caps.size(),other);
    for(auto& c:caps) std::printf("  POLL order_id=%llu local_order_id=%llu client_oid='%s'\n",
        (unsigned long long)c.order_id,(unsigned long long)c.local_id,c.coid);
}
int main(){
    config::KusakConfig cfg; cfg.symbol="BTCUSDT"; cfg.dry_run=false;
    const uint64_t L=1, E=987654321ULL;
    {
    std::printf("P1 UNKNOWN + borsa ID yok: POLL order_id yerel ID olmamali\n");
    auto P=fresh(cfg); StrategyContext& ctx=*P;
    setup_local(ctx,L);
    dispatch_order_result(ctx,mk(L,OrderStatus::ERROR,0,"HTTP"));
    auto* s=ctx.flat_order_map.find(L);
    CHECK(s && s->fsm_state==OrderState::UNKNOWN);
    CHECK(s && s->exchange_order_id==0);
    std::printf("  active_order_id=%llu awaiting_fill=%d kill_switch=%d\n",(unsigned long long)ctx.active_order_id,(int)ctx.awaiting_fill,(int)ctx.kill_switch);
    size_t left; long long ms; int other; auto caps=run_drain(ctx,700,left,ms,other); print_caps(caps,other,left,ms);
    std::printf("  yakalanan POLL sayisi=%zu\n",caps.size());   // [FIX-P35] sayi yalniz bilgi; kural tum elemanlar icin tek CHECK
    CHECK(std::all_of(caps.begin(),caps.end(),[&](const Cap& c){ return c.order_id!=L && (c.order_id!=0 || c.coid[0]!='\0'); }));
    }
    {
    std::printf("P2 SUBMITTED + borsa ID yok: POLL uretilmemeli\n");
    auto P=fresh(cfg); StrategyContext& ctx=*P;
    setup_local(ctx,L);
    auto* s=ctx.flat_order_map.find(L);
    CHECK(s && s->fsm_state==OrderState::SUBMITTED);
    CHECK(s && s->exchange_order_id==0);
    size_t left; long long ms; int other; auto caps=run_drain(ctx,700,left,ms,other); print_caps(caps,other,left,ms);
    CHECK(caps.empty());
    }
    {
    std::printf("P3 borsa ID mevcut: POLL order_id == E\n");
    auto P=fresh(cfg); StrategyContext& ctx=*P;
    setup_local(ctx,L);
    dispatch_order_result(ctx,mk(L,OrderStatus::NEW,E));
    auto* s=ctx.flat_order_map.find(L);
    CHECK(s && s->exchange_order_id==E);
    std::printf("  slot.exchange_order_id=%llu fsm_state=%d active_order_id=%llu fsm_key=%llu awaiting_fill=%d\n",
        s?(unsigned long long)s->exchange_order_id:0ULL, s?(int)s->fsm_state:-1,
        (unsigned long long)ctx.active_order_id,(unsigned long long)ctx.fsm_key,(int)ctx.awaiting_fill);
    size_t left; long long ms; int other; auto caps=run_drain(ctx,700,left,ms,other); print_caps(caps,other,left,ms);
    CHECK(!caps.empty());
    std::printf("  yakalanan POLL sayisi=%zu\n",caps.size());   // [FIX-P35]
    CHECK(std::all_of(caps.begin(),caps.end(),[&](const Cap& c){ return c.order_id==E; }));
    }
    {
    std::printf("P4 karakterizasyon: UNKNOWN + POLL HTTP400 hatasi -> UNKNOWN kalir, kilit/kill_switch korunur\n");
    auto P=fresh(cfg); StrategyContext& ctx=*P;
    setup_local(ctx,L);
    dispatch_order_result(ctx,mk(L,OrderStatus::ERROR,0,"HTTP"));
    dispatch_order_result(ctx,mk(L,OrderStatus::ERROR,0,"HTTP400"));
    auto* s=ctx.flat_order_map.find(L);
    CHECK(s && s->fsm_state==OrderState::UNKNOWN);
    CHECK(ctx.awaiting_fill==true);
    CHECK(ctx.kill_switch==true);
    CHECK(ctx.flat_order_map.active_count()>0);
    }
    // ═════════════════════════════════════════════════════════════════════════
    // [FIX-P0-02-TEST] Duzeltme testleri (P1-P4 yukarida degismedi)
    // ═════════════════════════════════════════════════════════════════════════
    std::printf("\n[FIX-P0-02-TEST]\n");
    {
    std::printf("B1 build_poll_task kimlik kurali\n");
    // (a) borsa ID var -> order_id==E, client_oid bos
    { auto P=fresh(cfg); StrategyContext& ctx=*P; setup_local(ctx,L);
      ctx.active_client_oid.set("KUSAK_123",9);
      dispatch_order_result(ctx,mk(L,OrderStatus::NEW,E));
      OrderTask pt{}; bool ok=build_poll_task(ctx,pt);
      CHECK(ok); CHECK(pt.kind==TaskKind::POLL); CHECK(pt.order_id==E); CHECK(pt.client_oid.empty()); CHECK(pt.local_order_id==L); }
    // (b) UNKNOWN + exchange 0 + coid -> order_id==0, coid
    { auto P=fresh(cfg); StrategyContext& ctx=*P; setup_local(ctx,L);
      ctx.active_client_oid.set("KUSAK_123",9);
      dispatch_order_result(ctx,mk(L,OrderStatus::ERROR,0,"HTTP"));
      auto* s=ctx.flat_order_map.find(L); CHECK(s && s->fsm_state==OrderState::UNKNOWN && s->exchange_order_id==0);
      OrderTask pt{}; bool ok=build_poll_task(ctx,pt);
      CHECK(ok); CHECK(pt.order_id==0); CHECK(std::strcmp(pt.client_oid.c_str(),"KUSAK_123")==0); }
    // (c) SUBMITTED + exchange 0 (coid dolu) -> false
    { auto P=fresh(cfg); StrategyContext& ctx=*P; setup_local(ctx,L);
      ctx.active_client_oid.set("KUSAK_123",9);
      OrderTask pt{}; CHECK(!build_poll_task(ctx,pt)); }
    // (d) fsm_key yok -> false ; slot yok -> false
    { auto P=fresh(cfg); StrategyContext& ctx=*P; OrderTask pt{}; CHECK(!build_poll_task(ctx,pt));
      ctx.fsm_key=77; ctx.awaiting_fill=true; CHECK(!build_poll_task(ctx,pt)); }
    // (e) UNKNOWN + exchange 0 + coid bos -> false
    { auto P=fresh(cfg); StrategyContext& ctx=*P; setup_local(ctx,L);
      dispatch_order_result(ctx,mk(L,OrderStatus::ERROR,0,"HTTP"));
      OrderTask pt{}; CHECK(!build_poll_task(ctx,pt)); }
    }
    {
    std::printf("B2 make_poll_query\n");
    char buf[160]; OrderTask t{}; t.symbol.set("BTCUSDT",7);
    t.order_id=987654321ULL;
    CHECK(make_poll_query(t,buf,sizeof buf)); CHECK(std::strcmp(buf,"symbol=BTCUSDT&orderId=987654321")==0);
    OrderTask c{}; c.symbol.set("BTCUSDT",7); c.client_oid.set("KUSAK_EXIT_123-a",16);
    CHECK(make_poll_query(c,buf,sizeof buf)); CHECK(std::strcmp(buf,"symbol=BTCUSDT&origClientOrderId=KUSAK_EXIT_123-a")==0);
    OrderTask n{}; n.symbol.set("BTCUSDT",7);
    CHECK(!make_poll_query(n,buf,sizeof buf));
    OrderTask bad{}; bad.symbol.set("BTCUSDT",7); bad.client_oid.set("&x=1",4);
    CHECK(!make_poll_query(bad,buf,sizeof buf));
    char small[16]; CHECK(!make_poll_query(t,small,sizeof small)); CHECK(!make_poll_query(c,small,sizeof small));
    CHECK(!make_poll_query(t,nullptr,0));
    }
    {
    std::printf("B3 belirsiz hatalar UNKNOWN'u korur (HTTP400_2013, NET_NOID)\n");
    for(const char* em : {"HTTP400_2013","NET_NOID"}){
      std::printf("  hata=%s\n",em);
      auto P=fresh(cfg); StrategyContext& ctx=*P; setup_local(ctx,L);
      dispatch_order_result(ctx,mk(L,OrderStatus::ERROR,0,"HTTP"));
      dispatch_order_result(ctx,mk(L,OrderStatus::ERROR,0,em));
      auto* s=ctx.flat_order_map.find(L);
      CHECK(s!=nullptr); CHECK(s && s->fsm_state==OrderState::UNKNOWN);
      CHECK(ctx.awaiting_fill==true); CHECK(ctx.kill_switch==true);
    }
    }
    {
    std::printf("B4 gecikmeli SEND yaniti: SUBMITTED'da POLL yok, NEW sonrasi POLL order_id==E\n");
    auto P=fresh(cfg); StrategyContext& ctx=*P; setup_local(ctx,L);
    ctx.active_client_oid.set("KUSAK_123",9);
    size_t left; long long ms; int other; auto caps=run_drain(ctx,700,left,ms,other); print_caps(caps,other,left,ms);
    CHECK(caps.empty());
    dispatch_order_result(ctx,mk(L,OrderStatus::NEW,E));
    caps=run_drain(ctx,700,left,ms,other); print_caps(caps,other,left,ms);
    CHECK(!caps.empty());
    std::printf("  yakalanan POLL sayisi=%zu\n",caps.size());   // [FIX-P35]
    CHECK(std::all_of(caps.begin(),caps.end(),[&](const Cap& c){ return c.order_id==E && c.local_id==L; }));
    }
    {
    std::printf("B5 client_oid yasam dongusu (EXIT: gercek send_exit_order; GIRIS yolu test edilemedi)\n");
    auto P=fresh(cfg); StrategyContext& ctx=*P;
    ctx.live_order_client.emplace(ctx.filter_validator, nullptr);   // ag yok: http_func bos; EXIT yalniz ring'e yazar
    ctx.position.side=PositionSide::LONG; ctx.position.qty=1.0; ctx.position.entry_price=100.0; ctx.position.notional=100.0;
    ctx.exit_retry_not_before_ns=0;
    send_exit_order(ctx,100.0,"B5");
    OrderTask t{}; bool got=ctx.order_channel.pop_task(t);
    CHECK(got); CHECK(t.kind==TaskKind::EXIT); CHECK(!t.client_oid.empty());
    CHECK(std::strcmp(ctx.active_client_oid.c_str(),t.client_oid.c_str())==0);
    const uint64_t K=ctx.fsm_key; CHECK(K>=FSM_EXIT_BASE);
    dispatch_order_result(ctx,mk(K,OrderStatus::NEW,E));   // is_exit sonucu: slot'tan bilinir
    OrderResult f=mk(K,OrderStatus::FILLED,E); f.is_exit=true; f.filled_qty=1.0; f.avg_price=100.0; f.remaining_qty=0.0;
    dispatch_order_result(ctx,f);
    CHECK(ctx.active_client_oid.empty());
    }
    std::printf("\nSONUC fails=%d\n",fails);
    return fails?1:0;
}
