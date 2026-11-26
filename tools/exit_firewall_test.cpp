// [P0-03-TEST] KZ-P0-03 kirmizi test. Derleme: g++ -std=c++20 -O2 -Wall -Wextra -Iinclude tools/exit_firewall_test.cpp -o ~/kusak_build/tools/exit_firewall_test -lssl -lcrypto -lcurl -lpthread
// Amac: azaltici EXIT emrinin (gercek send_exit_order / ks_service uretimi) final_order_firewall tarafindan PASS almasini dogrulamak.
// Beklenti: E1, E2, E4 mevcut kodda FAIL (KZ-P0-03 kaniti); E3 regresyon (yesil); E5 teshis (CHECK yok).
// Kapsam siniri: src/main.cpp icindeki firewall cagrisi (order thread) bu testten cagrilamaz; firewall dogrudan cagrilir.
#include <cstdint>
uint64_t g_tsc_ns_mult = (1ULL<<32);
#include "strategy_core.hpp"
#include <cstdio>
#include <memory>
#include <cstring>
#include <cmath>
#include <limits>
using namespace kusak;
static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n",__LINE__,#c); ++fails;} else std::printf("  ok: %s\n",#c);}while(0)

static const char* vname(FwVerdict v){
    switch(v){
        case FwVerdict::PASS: return "PASS"; case FwVerdict::BAD_INPUT: return "BAD_INPUT";
        case FwVerdict::POSITION_CAP: return "POSITION_CAP"; case FwVerdict::ORDER_CAP: return "ORDER_CAP";
        case FwVerdict::SYMBOL_MISMATCH: return "SYMBOL_MISMATCH"; case FwVerdict::FILTER: return "FILTER";
        case FwVerdict::EXPOSURE_CAP: return "EXPOSURE_CAP"; case FwVerdict::INSUFFICIENT_BALANCE: return "INSUFFICIENT_BALANCE";
        case FwVerdict::EXIT_POS_UNKNOWN: return "EXIT_POS_UNKNOWN"; case FwVerdict::EXIT_NOT_REDUCING: return "EXIT_NOT_REDUCING";   // [FIX-P0-03-TEST]
    }
    return "?";
}
// FlatOrderMap::slots[] varsayilan ilklenmiyor; her taze context'te slotlar sifirlanir (kaynak degismez).
static std::unique_ptr<StrategyContext> fresh(const config::KusakConfig& c){
    auto p=std::make_unique<StrategyContext>(c);
    std::memset(static_cast<void*>(p->flat_order_map.slots),0,sizeof(p->flat_order_map.slots));
    // Filtre: BTCUSDT benzeri (step 0.00001, tick 0.01, minQty 0.00001, minNotional 5)
    exchange::SymbolFilterCache fc; fc.symbol=c.symbol; fc.tick_size=0.01; fc.step_size=0.00001;
    fc.min_qty=0.00001; fc.max_qty=9000.0; fc.min_notional=5.0; fc.min_price=0.01; fc.max_price=1e9;
    p->filter_validator.update_filter(fc);
    fw_publish_limits(*p);
    p->live_order_client.emplace(p->filter_validator, nullptr);   // ag yok; EXIT yalniz ring'e yazar
    p->exit_retry_not_before_ns=0;
    return p;
}
static config::KusakConfig mkcfg(const char* market){
    config::KusakConfig cfg; cfg.symbol="BTCUSDT"; cfg.dry_run=false; cfg.market=market;
    cfg.max_position_usd=30.0; cfg.ks.max_order_notional_usd=30.0;
    cfg.ks.lock_file="/tmp/p0_03_test_kill.lock";   // ks_latch kilit dosyasini depo disina yazsin
    return cfg;
}
static void open_pos(StrategyContext& ctx, PositionSide s, double px, double qty){
    ctx.position.side=s; ctx.position.qty=qty; ctx.position.entry_price=px; ctx.position.notional=qty*px;
}
// [FIX-P0-03-TEST] Gercek yol: giris slotu (SUBMITTED) + dispatch_order_result ile borsa onayli sonuc -> ctx.position + yayin.
static OrderResult mk_res(uint64_t local, OrderStatus st, uint64_t ex, double filled, double avg, bool is_exit, bool is_buy, const char* err=nullptr){
    OrderResult r{}; r.local_order_id=local; r.order_id=local; r.exchange_order_id=ex; r.status=st;
    r.filled_qty=filled; r.avg_price=avg; r.is_exit=is_exit; r.is_buy=is_buy; r.ts_recv_ns=tsc_ns();
    if(err) r.error_msg.set(err,(uint8_t)std::strlen(err));
    return r;
}
static uint64_t mk_entry_slot(StrategyContext& ctx, uint64_t L, double qty, bool is_buy){
    ctx.fsm_entry_seq=L; ctx.flat_order_map.insert(L,qty);
    for(auto st:{OrderState::CREATED,OrderState::PREFLIGHT,OrderState::SUBMITTED}) ctx.flat_order_map.transition_key(L,st);
    ctx.fsm_key=L; ctx.active_order_id=L; ctx.awaiting_fill=true; ctx.pending_is_buy=is_buy;
    return L;
}
static void open_real(StrategyContext& ctx, bool is_buy, double px, double qty, uint64_t L=1, uint64_t ex=9001){
    mk_entry_slot(ctx,L,qty,is_buy);
    dispatch_order_result(ctx,mk_res(L,OrderStatus::FILLED,ex,qty,px,false,is_buy));
}
static OrderTask mk_task(TaskKind k, OrderSide s, double px, double qty, const char* sym="BTCUSDT"){
    OrderTask t{}; t.kind=k; t.side=s; t.type=OrderType::MARKET; t.price=px; t.qty=qty;
    t.is_exit=(k==TaskKind::EXIT); t.symbol.set(sym,(uint8_t)std::strlen(sym)); return t;
}
int main(){
    {
    std::printf("E1 SPOT LONG ~29 USD, SELL EXIT firewall tarafindan PASS almali\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.29);   // [FIX-P0-03] gercek yol: FILLED giris -> verified=true
    ctx.fw_position_usd.store(29.0); ctx.fw_free_quote_usd.store(1000.0);
    send_exit_order(ctx,100.0,"E1");
    OrderTask t{}; bool got=ctx.order_channel.pop_task(t);
    CHECK(got); CHECK(t.kind==TaskKind::EXIT); CHECK(t.side==OrderSide::SELL);
    FwVerdict v=final_order_firewall(ctx,t);
    std::printf("  FwVerdict=%s notional=%.4f fw_position_usd=%.2f\n",vname(v),t.qty*t.price,ctx.fw_position_usd.load());
    CHECK(v==FwVerdict::PASS);
    }
    {
    std::printf("E2 FUTURES SHORT ~29 USD, BUY EXIT firewall tarafindan PASS almali (free_quote=1)\n");
    auto cfg=mkcfg("FUTURES"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,false,100.0,0.29);   // [FIX-P0-03] gercek yol: FUTURES SHORT (SELL FILLED)
    ctx.fw_position_usd.store(29.0); ctx.fw_free_quote_usd.store(1.0);
    send_exit_order(ctx,100.0,"E2");
    OrderTask t{}; bool got=ctx.order_channel.pop_task(t);
    CHECK(got); CHECK(t.kind==TaskKind::EXIT); CHECK(t.side==OrderSide::BUY);
    FwVerdict v=final_order_firewall(ctx,t);
    std::printf("  FwVerdict=%s notional=%.4f fw_position_usd=%.2f free_quote=%.2f\n",vname(v),t.qty*t.price,ctx.fw_position_usd.load(),ctx.fw_free_quote_usd.load());
    CHECK(v==FwVerdict::PASS);
    }
    {
    std::printf("E3 regresyon (yesil beklenir)\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    ctx.fw_position_usd.store(29.0); ctx.fw_free_quote_usd.store(1000.0);
    FwVerdict v;
    v=final_order_firewall(ctx,mk_task(TaskKind::SEND,OrderSide::BUY,100.0,0.05));
    std::printf("  SEND BUY notional 5, pos=29 -> %s\n",vname(v)); CHECK(v==FwVerdict::EXPOSURE_CAP);
    v=final_order_firewall(ctx,mk_task(TaskKind::SEND,OrderSide::BUY,100.0,0.31));
    std::printf("  SEND notional 31 -> %s\n",vname(v)); CHECK(v==FwVerdict::POSITION_CAP);
    v=final_order_firewall(ctx,mk_task(TaskKind::SEND,OrderSide::BUY,100.0,std::numeric_limits<double>::quiet_NaN()));
    std::printf("  SEND qty=NaN -> %s\n",vname(v)); CHECK(v==FwVerdict::BAD_INPUT);
    v=final_order_firewall(ctx,mk_task(TaskKind::EXIT,OrderSide::SELL,100.0,std::numeric_limits<double>::quiet_NaN()));
    std::printf("  EXIT qty=NaN -> %s\n",vname(v)); CHECK(v==FwVerdict::BAD_INPUT);
    v=final_order_firewall(ctx,mk_task(TaskKind::EXIT,OrderSide::SELL,100.0,0.1,"ETHUSDT"));
    std::printf("  EXIT yanlis sembol -> %s\n",vname(v)); CHECK(v==FwVerdict::SYMBOL_MISMATCH);
    }
    {
    std::printf("E4 kill switch flatten EXIT firewall tarafindan PASS almali\n");
    auto cfg=mkcfg("SPOT"); cfg.ks.flatten_on_trip=true; auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.29);   // [FIX-P0-03] gercek yol
    ctx.fw_position_usd.store(29.0); ctx.fw_free_quote_usd.store(1000.0);
    ctx.awaiting_fill=false; ctx.last_bid=100.0; ctx.last_ask=100.01; ctx.last_mid=100.0;
    ks_latch(ctx,"TEST_TRIP",0.0,0.0);   // k.last_flatten_ns==0 -> 5 s kosulu ilk cagrida saglanir, zaman ayari gerekmedi
    ks_service(ctx);
    OrderTask t{}; bool got=ctx.order_channel.pop_task(t);
    std::printf("  EXIT gorevi uretildi=%d\n",(int)got);
    CHECK(got);
    if(got){ FwVerdict v=final_order_firewall(ctx,t);
        std::printf("  FwVerdict=%s kind=%d side=%s notional=%.4f\n",vname(v),(int)t.kind,t.side==OrderSide::SELL?"SELL":"BUY",t.qty*t.price);
        CHECK(t.kind==TaskKind::EXIT); CHECK(v==FwVerdict::PASS); }
    else std::printf("  NEDEN: ks_service EXIT uretmedi\n");
    }
    {
    std::printf("E5 teshis: pozisyon 15 USD, fw_position_usd guncellenmedi\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_pos(ctx,PositionSide::LONG,100.0,0.29);
    ctx.fw_position_usd.store(29.0); ctx.fw_free_quote_usd.store(1000.0);
    ctx.position.qty=0.15; ctx.position.notional=15.0;   // fw_publish_exposure cagrilmaz
    send_exit_order(ctx,100.0,"E5");
    OrderTask t{}; bool got=ctx.order_channel.pop_task(t);
    std::printf("  got=%d fw_position_usd=%.2f gercek_notional=%.4f gercek_pozisyon=%.2f verdict=%s\n",
        (int)got,ctx.fw_position_usd.load(),got?t.qty*t.price:0.0,ctx.position.qty*100.0,got?vname(final_order_firewall(ctx,t)):"-");
    }
    // ===================== [FIX-P0-03-TEST] =====================
    std::printf("\n[FIX-P0-03-TEST] E1-E5 yukarida (degismedi). Asagida X1-X5.\n");
    {
    std::printf("X1 EXIT ret/gecis testleri (pozisyon kopyasi fw_publish_exposure ile, verified=true)\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_pos(ctx,PositionSide::LONG,100.0,0.29);
    fw_publish_exposure(ctx,FwVerify::EVIDENCE);
    CHECK(ctx.fw_pos_side.load()==1); CHECK(std::fabs(ctx.fw_pos_qty.load()-0.29)<1e-12); CHECK(ctx.fw_pos_verified.load());
    FwVerdict v;
    v=final_order_firewall(ctx,mk_task(TaskKind::EXIT,OrderSide::SELL,100.0,0.29));
    std::printf("  X1 LONG SELL EXIT tam qty -> %s\n",vname(v)); CHECK(v==FwVerdict::PASS);
    v=final_order_firewall(ctx,mk_task(TaskKind::EXIT,OrderSide::SELL,100.0,0.29002));
    std::printf("  X1 EXIT qty=pos+2step -> %s\n",vname(v)); CHECK(v==FwVerdict::EXIT_NOT_REDUCING);
    v=final_order_firewall(ctx,mk_task(TaskKind::EXIT,OrderSide::BUY,100.0,0.1));
    std::printf("  X1 LONG iken BUY EXIT -> %s\n",vname(v)); CHECK(v==FwVerdict::EXIT_NOT_REDUCING);
    v=final_order_firewall(ctx,mk_task(TaskKind::EXIT,OrderSide::SELL,100.0,std::numeric_limits<double>::quiet_NaN()));
    std::printf("  X1 EXIT qty=NaN -> %s\n",vname(v)); CHECK(v==FwVerdict::BAD_INPUT);
    { OrderTask t=mk_task(TaskKind::EXIT,OrderSide::SELL,100.0,0.1); t.is_exit=false;
      v=final_order_firewall(ctx,t);
      std::printf("  X1 kind=EXIT is_exit=false -> %s\n",vname(v)); CHECK(v==FwVerdict::EXIT_NOT_REDUCING); }
    v=final_order_firewall(ctx,mk_task(TaskKind::EXIT,OrderSide::SELL,100.0,0.1,"ETHUSDT"));
    std::printf("  X1 EXIT yanlis sembol -> %s\n",vname(v)); CHECK(v==FwVerdict::SYMBOL_MISMATCH);
    // pozisyon yok (verified=true ama side=0)
    ctx.position.reset(); fw_publish_exposure(ctx,FwVerify::EVIDENCE);
    v=final_order_firewall(ctx,mk_task(TaskKind::EXIT,OrderSide::SELL,100.0,0.1));
    std::printf("  X1 pozisyon NONE -> %s\n",vname(v)); CHECK(v==FwVerdict::EXIT_POS_UNKNOWN);
    }
    {
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_pos(ctx,PositionSide::LONG,100.0,0.29);
    fw_publish_exposure(ctx,FwVerify::KEEP);   // verified hic yukseltilmedi
    CHECK(!ctx.fw_pos_verified.load());
    FwVerdict v=final_order_firewall(ctx,mk_task(TaskKind::EXIT,OrderSide::SELL,100.0,0.29));
    std::printf("  X1 verified=false -> %s\n",vname(v)); CHECK(v==FwVerdict::EXIT_POS_UNKNOWN);
    }
    {
    std::printf("X1b FUTURES SHORT BUY EXIT: free_quote=0 iken de PASS (azaltici)\n");
    auto cfg=mkcfg("FUTURES"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_pos(ctx,PositionSide::SHORT,100.0,0.29); ctx.current_capital=0.0;
    fw_publish_exposure(ctx,FwVerify::EVIDENCE);
    FwVerdict v=final_order_firewall(ctx,mk_task(TaskKind::EXIT,OrderSide::BUY,100.0,0.29));
    std::printf("  X1b -> %s free_quote=%.2f\n",vname(v),ctx.fw_free_quote_usd.load()); CHECK(v==FwVerdict::PASS);
    v=final_order_firewall(ctx,mk_task(TaskKind::EXIT,OrderSide::SELL,100.0,0.1));
    std::printf("  X1b SHORT iken SELL EXIT -> %s\n",vname(v)); CHECK(v==FwVerdict::EXIT_NOT_REDUCING);
    }
    {
    std::printf("X2 SEND regresyon (INSUFFICIENT_BALANCE, ORDER_CAP)\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    ctx.fw_position_usd.store(0.0); ctx.fw_free_quote_usd.store(10.0);
    FwVerdict v=final_order_firewall(ctx,mk_task(TaskKind::SEND,OrderSide::BUY,100.0,0.2));
    std::printf("  SEND BUY notional 20 > free_quote 10 -> %s\n",vname(v)); CHECK(v==FwVerdict::INSUFFICIENT_BALANCE);
    auto cfg2=mkcfg("SPOT"); cfg2.ks.max_order_notional_usd=10.0; auto P2=fresh(cfg2); StrategyContext& c2=*P2;
    c2.fw_position_usd.store(0.0); c2.fw_free_quote_usd.store(1000.0);
    v=final_order_firewall(c2,mk_task(TaskKind::SEND,OrderSide::BUY,100.0,0.2));
    std::printf("  SEND notional 20 > order_cap 10 (pos_cap 30) -> %s\n",vname(v)); CHECK(v==FwVerdict::ORDER_CAP);
    }
    {
    std::printf("X3 reduceOnly karari\n");
    OrderTask ex=mk_task(TaskKind::EXIT,OrderSide::SELL,100.0,0.1), sd=mk_task(TaskKind::SEND,OrderSide::BUY,100.0,0.1);
    CHECK(exit_needs_reduce_only(ex,true)); CHECK(!exit_needs_reduce_only(ex,false)); CHECK(!exit_needs_reduce_only(sd,true));
    std::printf("  build_order_params private: sorgu dizgisi (reduceOnly=true) bu testte TEST EDILMEDI\n");
    }
    {
    std::printf("X4 uctan uca flatten: ks_service EXIT -> firewall PASS -> FILLED -> pozisyon kapanir\n");
    auto cfg=mkcfg("SPOT"); cfg.ks.flatten_on_trip=true; auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_pos(ctx,PositionSide::LONG,100.0,0.29);
    fw_publish_exposure(ctx,FwVerify::EVIDENCE);
    ctx.awaiting_fill=false; ctx.last_bid=100.0; ctx.last_ask=100.01; ctx.last_mid=100.0;
    ks_latch(ctx,"TEST_TRIP",0.0,0.0);
    ks_service(ctx);
    OrderTask t{}; bool got=ctx.order_channel.pop_task(t);
    CHECK(got);
    if(got){
        FwVerdict v=final_order_firewall(ctx,t);
        std::printf("  FwVerdict=%s\n",vname(v)); CHECK(t.kind==TaskKind::EXIT); CHECK(v==FwVerdict::PASS);
        OrderResult r{}; r.order_id=t.local_order_id; r.local_order_id=t.local_order_id; r.exchange_order_id=t.local_order_id;
        r.is_exit=true; r.status=OrderStatus::FILLED; r.filled_qty=t.qty; r.remaining_qty=0.0; r.avg_price=100.0;
        r.ts_recv_ns=ctx.current_time_ns();
        dispatch_order_result(ctx,r);
        std::printf("  is_open=%d fw_pos_side=%d fw_position_usd=%.4f\n",(int)ctx.position.is_open(),(int)ctx.fw_pos_side.load(),ctx.fw_position_usd.load());
        CHECK(!ctx.position.is_open()); CHECK(ctx.fw_pos_side.load()==0); CHECK(ctx.fw_position_usd.load()==0.0);
    }
    }
    {
    std::printf("X5 bayat maruziyet: send_exit_order sonrasi fw_position_usd gercek pozisyonu yansitmali\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_pos(ctx,PositionSide::LONG,100.0,0.29);
    ctx.fw_position_usd.store(29.0); ctx.fw_free_quote_usd.store(1000.0);
    ctx.position.qty=0.15; ctx.position.notional=15.0;
    send_exit_order(ctx,100.0,"X5");
    OrderTask t{}; bool got=ctx.order_channel.pop_task(t); CHECK(got);
    std::printf("  fw_position_usd=%.4f fw_pos_qty=%.4f\n",ctx.fw_position_usd.load(),ctx.fw_pos_qty.load());
    CHECK(std::fabs(ctx.fw_position_usd.load()-15.0)<=0.01);
    }

    // ===================== [FIX-P0-03-TEST-V] =====================
    std::printf("\n[FIX-P0-03-TEST-V] verified kurali (gercek dispatch_order_result yolu)\n");
    {
    std::printf("V1 acik pozisyon (verified) -> EXIT icin ERROR HTTP -> slot UNKNOWN -> verified=false -> EXIT_POS_UNKNOWN\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.29); CHECK(ctx.fw_pos_verified.load());
    send_exit_order(ctx,100.0,"V1"); OrderTask t{}; bool got=ctx.order_channel.pop_task(t); CHECK(got);
    CHECK(final_order_firewall(ctx,t)==FwVerdict::PASS);
    dispatch_order_result(ctx,mk_res(t.local_order_id,OrderStatus::ERROR,0,0.0,0.0,true,false,"HTTP"));
    const OrderSlot* sl=ctx.flat_order_map.find(t.local_order_id);
    CHECK(sl && sl->fsm_state==OrderState::UNKNOWN); CHECK(!ctx.fw_pos_verified.load());
    FwVerdict v=final_order_firewall(ctx,t); std::printf("  V1 -> %s\n",vname(v)); CHECK(v==FwVerdict::EXIT_POS_UNKNOWN);
    std::printf("V2 UNKNOWN -> borsa onayli CANCELED (filled 0) -> verified=true -> EXIT PASS\n");
    dispatch_order_result(ctx,mk_res(t.local_order_id,OrderStatus::CANCELED,7777,0.0,0.0,true,false));
    CHECK(ctx.position.is_open()); CHECK(ctx.fw_pos_verified.load());
    v=final_order_firewall(ctx,t); std::printf("  V2 -> %s\n",vname(v)); CHECK(v==FwVerdict::PASS);
    }
    {
    std::printf("V3 UNKNOWN -> borsa onayli EXIT FILLED (tam) -> pozisyon kapali, verified=false, side=0\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.29);
    send_exit_order(ctx,100.0,"V3"); OrderTask t{}; bool got=ctx.order_channel.pop_task(t); CHECK(got);
    dispatch_order_result(ctx,mk_res(t.local_order_id,OrderStatus::ERROR,0,0.0,0.0,true,false,"HTTP"));
    CHECK(!ctx.fw_pos_verified.load());
    dispatch_order_result(ctx,mk_res(t.local_order_id,OrderStatus::FILLED,7778,t.qty,100.0,true,false));
    CHECK(!ctx.position.is_open()); CHECK(!ctx.fw_pos_verified.load()); CHECK(ctx.fw_pos_side.load()==0);
    }
    {
    std::printf("V4 verified=true iken yerel REJECTED FW_REJECT (exchange_order_id=0) -> verified true kalir\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.29); CHECK(ctx.fw_pos_verified.load());
    send_exit_order(ctx,100.0,"V4"); OrderTask t{}; bool got=ctx.order_channel.pop_task(t); CHECK(got);
    dispatch_order_result(ctx,mk_res(t.local_order_id,OrderStatus::REJECTED,0,0.0,0.0,true,false,"FW_REJECT"));
    CHECK(ctx.position.is_open()); CHECK(ctx.fw_pos_verified.load());
    }
    {
    std::printf("V5 giris kismi dolumla CANCELED (filled_qty>0) -> pozisyon acik, verified=true\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    mk_entry_slot(ctx,1,0.29,true); ctx.flat_order_map.transition_key(1,OrderState::NEW);   // borsa ACK/NEW gorulmus emir (SUBMITTED->CANCELED gecersiz)
    OrderResult r=mk_res(1,OrderStatus::CANCELED,9002,0.15,100.0,false,true); r.remaining_qty=0.14;
    dispatch_order_result(ctx,r);
    std::printf("  is_open=%d qty=%.4f verified=%d\n",(int)ctx.position.is_open(),ctx.position.qty,(int)ctx.fw_pos_verified.load());
    CHECK(ctx.position.is_open()); CHECK(ctx.fw_pos_verified.load());
    }

    std::printf("\nSONUC fails=%d\n",fails);
    return fails?1:0;
}
