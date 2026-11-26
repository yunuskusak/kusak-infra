// [P35-TEST] Faz 3.5 kirmizi testler. Derleme: g++ -std=c++20 -O2 -Wall -Wextra -Iinclude tools/phase35_test.cpp -o ~/kusak_build/tools/phase35_test -lssl -lcrypto -lcurl -lpthread
// Amac: R1 FlatOrderMap ilklenmemesi (C12), R2 EXIT qty hizalama/azaltici kontrolu, R3 floor_qty olcumu.
// Beklenti: R1 (ilk 2 CHECK) ve R2a, R2b mevcut kodda FAIL; R2c yesil; R3 sonucu ne cikarsa kaydedilir.
// Kapsam siniri: src/main.cpp firewall cagrisi bu testten cagrilamaz; firewall dogrudan cagrilir. Kaynak degismez.
#include <cstdint>
uint64_t g_tsc_ns_mult = (1ULL<<32);
#include "strategy_core.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <cstring>
#include <cmath>
#include <limits>
#include <string>
using namespace kusak;
static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n",__LINE__,#c); ++fails;} else std::printf("  ok: %s\n",#c);}while(0)

static const char* vname(FwVerdict v){
    switch(v){
        case FwVerdict::PASS: return "PASS"; case FwVerdict::BAD_INPUT: return "BAD_INPUT";
        case FwVerdict::POSITION_CAP: return "POSITION_CAP"; case FwVerdict::ORDER_CAP: return "ORDER_CAP";
        case FwVerdict::SYMBOL_MISMATCH: return "SYMBOL_MISMATCH"; case FwVerdict::FILTER: return "FILTER";
        case FwVerdict::EXPOSURE_CAP: return "EXPOSURE_CAP"; case FwVerdict::INSUFFICIENT_BALANCE: return "INSUFFICIENT_BALANCE";
        case FwVerdict::EXIT_POS_UNKNOWN: return "EXIT_POS_UNKNOWN"; case FwVerdict::EXIT_NOT_REDUCING: return "EXIT_NOT_REDUCING";
    }
    return "?";
}
static void set_filter(StrategyContext& ctx, const config::KusakConfig& c, double step){
    exchange::SymbolFilterCache fc; fc.symbol=c.symbol; fc.tick_size=0.01; fc.step_size=step;
    fc.min_qty=0.00001; fc.max_qty=9000.0; fc.min_notional=5.0; fc.min_price=0.01; fc.max_price=1e9;
    ctx.filter_validator.update_filter(fc);
}
// Taze context: slot sifirlama YOK burada kasitli degil; diger senaryolar icin memset kullanilir (R1 haric).
static std::unique_ptr<StrategyContext> fresh(const config::KusakConfig& c, double step=0.00001){
    auto p=std::make_unique<StrategyContext>(c);
    std::memset(static_cast<void*>(p->flat_order_map.slots),0,sizeof(p->flat_order_map.slots));
    set_filter(*p,c,step);
    fw_publish_limits(*p);
    p->live_order_client.emplace(p->filter_validator, nullptr);
    p->exit_retry_not_before_ns=0;
    return p;
}
static config::KusakConfig mkcfg(const char* market){
    config::KusakConfig cfg; cfg.symbol="BTCUSDT"; cfg.dry_run=false; cfg.market=market;
    cfg.max_position_usd=30.0; cfg.ks.max_order_notional_usd=30.0;
    cfg.ks.lock_file="/tmp/p35_test_kill.lock";
    cfg.write_csv=false; cfg.write_prometheus=false;
    return cfg;
}
static OrderResult mk_res(uint64_t local, OrderStatus st, uint64_t ex, double filled, double avg, bool is_exit, bool is_buy){
    OrderResult r{}; r.local_order_id=local; r.order_id=local; r.exchange_order_id=ex; r.status=st;
    r.filled_qty=filled; r.avg_price=avg; r.is_exit=is_exit; r.is_buy=is_buy; r.ts_recv_ns=tsc_ns();
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
static OrderTask mk_exit(OrderSide s, double px, double qty){
    OrderTask t{}; t.kind=TaskKind::EXIT; t.side=s; t.type=OrderType::MARKET; t.price=px; t.qty=qty;
    t.is_exit=true; t.symbol.set("BTCUSDT",7); return t;
}
int main(){
    {
    std::printf("R1 FlatOrderMap ilklenmemis bellek (buffer 0x01 dolu, placement new, memset izolasyonu YOK)\n");
    auto cfg=mkcfg("SPOT");
    const size_t sz=sizeof(StrategyContext);
    void* buf=::operator new(sz,std::align_val_t(alignof(StrategyContext)));
    std::memset(buf,0x01,sz);
    auto* ctx=new (buf) StrategyContext(cfg);
    size_t bad=0;
    for(size_t i=0;i<MAX_ORDERS;++i){
        const OrderSlot& s=ctx->flat_order_map.slots[i];
        if(!(s.state==SlotState::EMPTY && s.gen==0 && s.exchange_order_id==0)) ++bad;
    }
    std::printf("  hatali slot sayisi=%zu / %zu (sizeof ctx=%zu)\n",bad,(size_t)MAX_ORDERS,sz);
    CHECK(bad==0);
    bool ins=ctx->flat_order_map.insert(1,0.01);
    std::printf("  insert(1)=%d find=%d consistent=%d active_slots=%zu active_count=%zu\n",(int)ins,
        (int)(ctx->flat_order_map.find(1)!=nullptr),(int)ctx->flat_order_map.consistent(),
        (size_t)ctx->flat_order_map.active_slots,(size_t)ctx->flat_order_map.active_count());
    CHECK(ctx->flat_order_map.find(1)!=nullptr);
    CHECK(ctx->flat_order_map.consistent());
    ctx->~StrategyContext();
    ::operator delete(buf,std::align_val_t(alignof(StrategyContext)));
    }
    {
    std::printf("R2a LONG qty=0.290006 (hizasiz), send_exit_order task.qty <= position.qty olmali\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.290006);
    send_exit_order(ctx,100.0,"R2a");
    OrderTask t{}; bool got=ctx.order_channel.pop_task(t);
    std::printf("  got=%d task.qty=%.17g position.qty=%.17g\n",(int)got,t.qty,ctx.position.qty);
    CHECK(got);
    CHECK(t.qty <= ctx.position.qty + 1e-12);
    }
    {
    std::printf("R2b LONG 0.29, elle EXIT qty=0.29001 -> EXIT_NOT_REDUCING beklenir\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.29);
    FwVerdict v=final_order_firewall(ctx,mk_exit(OrderSide::SELL,100.0,0.29001));
    std::printf("  verdict=%s fw_pos_qty=%.17g\n",vname(v),ctx.fw_pos_qty.load());
    CHECK(v==FwVerdict::EXIT_NOT_REDUCING);
    }
    {
    std::printf("R2c LONG 0.29, EXIT qty=0.29 -> PASS (yesil beklenir)\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.29);
    FwVerdict v=final_order_firewall(ctx,mk_exit(OrderSide::SELL,100.0,0.29));
    std::printf("  verdict=%s\n",vname(v));
    CHECK(v==FwVerdict::PASS);
    }
    {
    std::printf("R3 floor_qty olcumu\n");
    auto cfg=mkcfg("SPOT");
    { auto P=fresh(cfg,0.1); double r=P->filter_validator.floor_qty(cfg.symbol,0.3);
      std::printf("  step=0.1 floor_qty(0.3)=%.17g\n",r); CHECK(std::fabs(r-0.3)<1e-12); }
    { auto P=fresh(cfg,0.00001); double r=P->filter_validator.floor_qty(cfg.symbol,0.290006);
      std::printf("  step=0.00001 floor_qty(0.290006)=%.17g\n",r); CHECK(std::fabs(r-0.29)<1e-12); }
    }
    // ===================== [FIX-P35-TEST] Adim 3 duzeltme testleri (R1-R3 yukarida aynen) =====================
    {
    std::printf("A1 adopt_position SPOT LONG 0.29 @100\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    bool ok=adopt_position(ctx,PositionSide::LONG,0.29,100.0);
    CHECK(ok);
    CHECK(ctx.position.is_open() && ctx.position.side==PositionSide::LONG);
    CHECK(std::fabs(ctx.position.qty-0.29)<1e-12);
    CHECK(ctx.fw_pos_side.load()==1);
    CHECK(ctx.fw_pos_verified.load()==true);
    CHECK(std::fabs(ctx.fw_position_usd.load()-29.0)<1e-6);
    send_exit_order(ctx,100.0,"A1");
    OrderTask t{}; bool got=ctx.order_channel.pop_task(t);
    CHECK(got && t.kind==TaskKind::EXIT);
    FwVerdict v=got?final_order_firewall(ctx,t):FwVerdict::BAD_INPUT;
    std::printf("  verdict=%s task.qty=%.17g\n",vname(v),t.qty);
    CHECK(v==FwVerdict::PASS);
    }
    {
    std::printf("A2 adopt_position gecersiz girdi -> false, pozisyon acilmaz\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    CHECK(!adopt_position(ctx,PositionSide::LONG,0.0,100.0));
    CHECK(!adopt_position(ctx,PositionSide::LONG,std::numeric_limits<double>::quiet_NaN(),100.0));
    CHECK(!adopt_position(ctx,PositionSide::LONG,0.29,0.0));
    CHECK(!ctx.position.is_open());
    }
    {
    std::printf("A3 adopt_decision (main.cpp ADOPT dalinin saf mantigi; dalin kendisi main.cpp'de, test edilmedi)\n");
    CHECK(adopt_decision(true,0.29,0.0,0.00001)==AdoptDecision::REFUSE_FUTURES);
    CHECK(adopt_decision(false,0.29,0.00001,0.00001)==AdoptDecision::REFUSE_LOCKED);
    CHECK(adopt_decision(false,0.29,0.0,0.00001)==AdoptDecision::OK);
    }
    {
    std::printf("S1 seqlock: tek seq -> EXIT_POS_UNKNOWN, cift -> PASS\n");
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.29);
    const uint32_t even=ctx.fw_pos_seq.load();
    CHECK((even&1u)==0u);
    ctx.fw_pos_seq.store(even+1u);
    FwVerdict v1=final_order_firewall(ctx,mk_exit(OrderSide::SELL,100.0,0.29));
    std::printf("  tek seq -> %s\n",vname(v1));
    CHECK(v1==FwVerdict::EXIT_POS_UNKNOWN);
    ctx.fw_pos_seq.store(even);
    FwVerdict v2=final_order_firewall(ctx,mk_exit(OrderSide::SELL,100.0,0.29));
    std::printf("  cift seq -> %s\n",vname(v2));
    CHECK(v2==FwVerdict::PASS);
    }
    {
    std::printf("Q1 miktar toleransi (step=1e-5). NOT: step'e hizasiz EXIT qty'leri FILTER'a takilir (validate_order, tolerans kontrolunden ONCE);\n");
    std::printf("   bu yuzden istenen 0.29+0.5*step / 0.29+step*1e-4 probleri bilgi amaclidir, CHECK hizali esdegerlerle yapilir.\n");
    {
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.29);
    FwVerdict a=final_order_firewall(ctx,mk_exit(OrderSide::SELL,100.0,0.29+0.5*0.00001));
    FwVerdict b=final_order_firewall(ctx,mk_exit(OrderSide::SELL,100.0,0.29+0.00001*1e-4));
    std::printf("  (bilgi) pos 0.29: EXIT +0.5*step -> %s ; EXIT +step*1e-4 -> %s\n",vname(a),vname(b));
    }
    {
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.290006);   // hizasiz pozisyon
    FwVerdict a=final_order_firewall(ctx,mk_exit(OrderSide::SELL,100.0,0.29001));   // pos + 0.4*step, hizali
    std::printf("  pos 0.290006: EXIT 0.29001 (+0.4*step) -> %s\n",vname(a)); CHECK(a==FwVerdict::EXIT_NOT_REDUCING);
    FwVerdict b=final_order_firewall(ctx,mk_exit(OrderSide::SELL,100.0,0.29));
    std::printf("  pos 0.290006: EXIT 0.29 -> %s\n",vname(b)); CHECK(b==FwVerdict::PASS);
    }
    {
    auto cfg=mkcfg("SPOT"); auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.29-1e-9);   // pozisyon, hizali EXIT'ten step*1e-4 kucuk
    FwVerdict c=final_order_firewall(ctx,mk_exit(OrderSide::SELL,100.0,0.29));
    std::printf("  pos 0.29-1e-9: EXIT 0.29 (+step*1e-4) -> %s\n",vname(c)); CHECK(c==FwVerdict::PASS);
    }
    }
    {
    std::printf("D1 toz kalintisi: LONG 0.290006 gercek open_real, EXIT 0.29 FILLED enjekte\n");
    auto cfg=mkcfg("SPOT"); cfg.ks.flatten_on_trip=true; auto P=fresh(cfg); StrategyContext& ctx=*P;
    open_real(ctx,true,100.0,0.290006);
    ctx.awaiting_fill=false; ctx.last_bid=100.0; ctx.last_ask=100.01; ctx.last_mid=100.0;
    send_exit_order(ctx,100.0,"D1");
    OrderTask t{}; bool got=ctx.order_channel.pop_task(t);
    CHECK(got);
    std::printf("  task.qty=%.17g position.qty=%.17g\n",t.qty,ctx.position.qty);
    dispatch_order_result(ctx,mk_res(t.local_order_id,OrderStatus::FILLED,9002,t.qty,100.0,true,false));
    std::printf("  sonrasi is_open=%d position.qty=%.17g fw_pos_side=%d\n",(int)ctx.position.is_open(),ctx.position.qty,(int)ctx.fw_pos_side.load());
    CHECK(!ctx.position.is_open());
    CHECK(ctx.fw_pos_side.load()==0);
    ks_latch(ctx,"D1_TRIP",0.0,0.0);
    ks_service(ctx);
    int exits=0; OrderTask x{}; while(ctx.order_channel.pop_task(x)) if(x.kind==TaskKind::EXIT) ++exits;
    std::printf("  ks_service sonrasi EXIT task sayisi=%d\n",exits);
    CHECK(exits==0);
    }
    {
    std::printf("RO1-RO3 reduceOnly ham sorgu\n");
    auto mkreq=[&](const OrderTask& t, bool futures){
        kusak::exchange::RestOrderClient::OrderRequest req;
        req.symbol=t.symbol.c_str(); req.side=(t.side==OrderSide::BUY)?"BUY":"SELL";
        req.type=(t.type==OrderType::LIMIT_MAKER)?"LIMIT_MAKER":(t.type==OrderType::LIMIT)?"LIMIT":"MARKET";
        req.time_in_force="GTC"; req.quantity=t.qty; req.client_oid=t.client_oid.c_str();
        if(t.type==OrderType::LIMIT||t.type==OrderType::LIMIT_MAKER) req.price=t.price;
        req.reduce_only=exit_needs_reduce_only(t,futures);
        return kusak::exchange::RestOrderClient::build_order_params(req);
    };
    OrderTask ex=mk_exit(OrderSide::SELL,100.0,0.29);
    OrderTask sd=mk_exit(OrderSide::BUY,100.0,0.29); sd.kind=TaskKind::SEND; sd.is_exit=false; sd.type=OrderType::LIMIT_MAKER;
    std::string q1=mkreq(ex,true), q2=mkreq(ex,false), q3=mkreq(sd,true);
    std::printf("  RO1 FUTURES EXIT: %s\n  RO2 SPOT EXIT: %s\n  RO3 FUTURES SEND: %s\n",q1.c_str(),q2.c_str(),q3.c_str());
    CHECK(q1.find("reduceOnly=true&")!=std::string::npos);   // RO1
    CHECK(q2.find("reduceOnly")==std::string::npos);         // RO2
    CHECK(q3.find("reduceOnly")==std::string::npos);         // RO3
    }
    std::printf("\nSONUC fails=%d\n",fails);
    return fails?1:0;
}
