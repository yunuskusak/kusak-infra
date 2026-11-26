#include <cstdint>
uint64_t g_tsc_ns_mult = (1ULL<<32);
#include "strategy_core.hpp"
#include <cstdio>
#include <thread>
#include <memory>
#include <cmath>
using namespace kusak;
#ifndef FSM_EXIT_BASE_LIT
#define FSM_EXIT_BASE_LIT (1ULL<<56)
#endif
static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n",__LINE__,#c); ++fails;} else std::printf("  ok: %s\n",#c);}while(0)

static OrderResult R(uint64_t local, OrderStatus st, bool exitflag, double fq, double rem, double px, uint64_t ex=0, bool buy=true){
    OrderResult r{}; r.local_order_id=local; r.order_id=ex?ex:local; r.exchange_order_id=ex; r.status=st;
    r.is_exit=exitflag; r.filled_qty=fq; r.remaining_qty=rem; r.avg_price=px; r.is_buy=buy; r.ts_recv_ns=tsc_ns(); return r; }
static void open_entry_live(StrategyContext& ctx, uint64_t key, uint64_t ex){
    ctx.flat_order_map.insert(key,1.0);
    for(auto s:{OrderState::CREATED,OrderState::PREFLIGHT,OrderState::SUBMITTED}) ctx.flat_order_map.transition_key(key,s);
    ctx.fsm_key=key; ctx.active_order_id=key; ctx.awaiting_fill=true; ctx.pending_is_buy=true;
    dispatch_order_result(ctx,R(key,OrderStatus::PENDING,false,0,1,0,ex));
}
int main(){
    config::KusakConfig cfg; cfg.symbol="BTCUSDT"; cfg.dry_run=false;
    {
    std::printf("T1 EXIT'in POLL'dan gelen FILLED'i (is_exit=false gelir) pozisyonu KAPATMALI\n");
    auto P_ctx=std::make_unique<StrategyContext>(cfg); StrategyContext& ctx=*P_ctx;
    open_entry_live(ctx,1,900);
    dispatch_order_result(ctx,R(1,OrderStatus::FILLED,false,1.0,0.0,100.0,900,true));
    CHECK(ctx.position.is_open()); CHECK(ctx.flat_order_map.active_count()==0);
    uint64_t xk=++ctx.fsm_exit_seq;
    ctx.flat_order_map.insert(xk,1.0);
    for(auto s:{OrderState::CREATED,OrderState::PREFLIGHT,OrderState::SUBMITTED}) ctx.flat_order_map.transition_key(xk,s);
    ctx.fsm_key=xk; ctx.awaiting_fill=true; ctx.active_order_id=0;
    dispatch_order_result(ctx,R(xk,OrderStatus::PENDING,false,0,1,0,901,false));
    dispatch_order_result(ctx,R(xk,OrderStatus::FILLED,/*is_exit YOK (POLL)*/false,1.0,0.0,101.0,901,false));
    CHECK(!ctx.position.is_open()); CHECK(!ctx.awaiting_fill); CHECK(ctx.flat_order_map.active_count()==0);
    }
#ifdef PATCHED
    {
    std::printf("T2 EXIT PARTIAL pozisyonu ezmemeli; sonra FILLED kapatmali\n");
    auto P_ctx=std::make_unique<StrategyContext>(cfg); StrategyContext& ctx=*P_ctx;
    open_entry_live(ctx,1,900);
    dispatch_order_result(ctx,R(1,OrderStatus::FILLED,false,2.0,0.0,100.0,900,true));
    double q0=ctx.position.qty;
    uint64_t xk=++ctx.fsm_exit_seq; ctx.flat_order_map.insert(xk,2.0);
    for(auto s:{OrderState::CREATED,OrderState::PREFLIGHT,OrderState::SUBMITTED}) ctx.flat_order_map.transition_key(xk,s);
    ctx.fsm_key=xk; ctx.awaiting_fill=true;
    dispatch_order_result(ctx,R(xk,OrderStatus::PARTIALLY_FILLED,true,0.5,1.5,101.0,902,false));
    CHECK(ctx.position.is_open()); CHECK(ctx.position.qty==q0);
    CHECK(ctx.flat_order_map.has_live_exit());
    dispatch_order_result(ctx,R(xk,OrderStatus::FILLED,true,2.0,0.0,101.0,902,false));
    CHECK(!ctx.position.is_open()); CHECK(!ctx.flat_order_map.has_live_exit()); CHECK(ctx.flat_order_map.consistent());
    }
    {
    std::printf("T3 cancel/fill yarisi: tek terminal, tek eksiltme; EXIT iptal edilmez\n");
    auto P_ctx=std::make_unique<StrategyContext>(cfg); StrategyContext& ctx=*P_ctx;
    open_entry_live(ctx,5,950);
    ctx.flat_order_map.step(*ctx.flat_order_map.find(5),OrderState::NEW);
    CHECK(request_entry_cancel(ctx,5,tsc_ns()));
    CHECK(ctx.flat_order_map.find(5)->fsm_state==OrderState::CANCEL_PENDING);
    CHECK(!request_entry_cancel(ctx,5,tsc_ns()));          // 2. iptal GITMEZ
    CHECK(!request_entry_cancel(ctx,(1ULL<<56)+1,tsc_ns())); // EXIT anahtari
    dispatch_order_result(ctx,R(5,OrderStatus::FILLED,false,1.0,0.0,100.0,950,true));   // yarisi fill kazandi
    CHECK(ctx.flat_order_map.active_count()==0); CHECK(ctx.position.is_open());
    dispatch_order_result(ctx,R(5,OrderStatus::CANCELED,false,0,1,0,950,true));         // gec cancel ack
    dispatch_order_result(ctx,R(5,OrderStatus::FILLED,false,1.0,0.0,100.0,950,true));   // duplicate fill
    CHECK(ctx.flat_order_map.active_count()==0); CHECK(ctx.flat_order_map.consistent());
    CHECK(ctx.position.qty==1.0);                          // cift sayim yok
    }
    {
    std::printf("T4 dry-run: giris terminal, exit kendi slotu; active_orders=0\n");
    config::KusakConfig dcfg=cfg; dcfg.dry_run=true;
    auto P_ctx=std::make_unique<StrategyContext>(dcfg); StrategyContext& ctx=*P_ctx;
    ctx.position.side=PositionSide::LONG; ctx.position.entry_price=100; ctx.position.qty=1; ctx.position.notional=100;
    send_exit_order(ctx,101.0,"test");
    CHECK(!ctx.position.is_open()); CHECK(ctx.flat_order_map.active_count()==0); CHECK(ctx.flat_order_map.consistent());
    }
    {
    std::printf("T5 shutdown drain: order thread cevap veriyor -> 0; cevap yok -> sinirli bekleme, zorla silmez\n");
    auto P_ctx=std::make_unique<StrategyContext>(cfg); StrategyContext& ctx=*P_ctx;
    open_entry_live(ctx,7,970);
    ctx.flat_order_map.step(*ctx.flat_order_map.find(7),OrderState::NEW);
    std::atomic<bool> run{true};
    std::thread fake([&]{ OrderTask t{}; while(run){ if(ctx.order_channel.pop_task(t)){ if(t.kind==TaskKind::CANCEL){ OrderResult r{}; r.order_id=t.order_id; r.local_order_id=t.local_order_id; r.status=OrderStatus::CANCELED; r.ts_recv_ns=tsc_ns(); ctx.order_channel.push_result(r);} } else std::this_thread::sleep_for(std::chrono::microseconds(200)); } });
    size_t left=drain_orders_on_shutdown(ctx,2000);
    run=false; fake.join();
    CHECK(left==0); CHECK(ctx.flat_order_map.active_count()==0);
    auto P_c2=std::make_unique<StrategyContext>(cfg); StrategyContext& c2=*P_c2;
    open_entry_live(c2,8,980); c2.flat_order_map.step(*c2.flat_order_map.find(8),OrderState::NEW);
    auto t0=std::chrono::steady_clock::now();
    size_t left2=drain_orders_on_shutdown(c2,400);
    auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
    CHECK(left2==1); CHECK(ms>=380 && ms<1500);
    }

    {
    std::printf("T6 EXIT 4/10 doldu, kalan EXPIRED/REJECTED/ERROR\n");
    for (int variant=0; variant<4; ++variant) {
        auto P6=std::make_unique<StrategyContext>(cfg); StrategyContext& ctx=*P6;
        ctx.current_capital=1000;
        open_entry_live(ctx,1,900);
        dispatch_order_result(ctx,R(1,OrderStatus::FILLED,false,10.0,0.0,100.0,900,true));
        uint64_t xk=++ctx.fsm_exit_seq; ctx.flat_order_map.insert(xk,10.0);
        for(auto s:{OrderState::CREATED,OrderState::PREFLIGHT,OrderState::SUBMITTED}) ctx.flat_order_map.transition_key(xk,s);
        ctx.fsm_key=xk; ctx.awaiting_fill=true;
        dispatch_order_result(ctx,R(xk,OrderStatus::PARTIALLY_FILLED,false,4.0,6.0,101.0,902,false));
        CHECK(ctx.position.qty==10.0);
        if(variant==0){ dispatch_order_result(ctx,R(xk,OrderStatus::EXPIRED,false,0.0,6.0,0.0,902,false));
            CHECK(ctx.position.is_open()); CHECK(std::abs(ctx.position.qty-6.0)<1e-9); CHECK(std::abs(ctx.current_capital-1004.0)<1e-9); CHECK(ctx.flat_order_map.active_count()==0); }
        if(variant==1){ dispatch_order_result(ctx,R(xk,OrderStatus::CANCELED,false,4.0,6.0,101.0,902,false));
            CHECK(std::abs(ctx.position.qty-6.0)<1e-9); CHECK(ctx.flat_order_map.active_count()==0); }
        if(variant==2){ OrderResult e=R(xk,OrderStatus::ERROR,false,0,0,0,902,false); e.error_msg.set("MIN_NOTIONAL",12);
            dispatch_order_result(ctx,e); CHECK(ctx.position.qty==10.0); CHECK(ctx.flat_order_map.active_count()==1); // PARTIAL->hata: UNKNOWN, reconcile bekler
            dispatch_order_result(ctx,R(xk,OrderStatus::EXPIRED,false,4.0,6.0,101.0,902,false));
            CHECK(std::abs(ctx.position.qty-6.0)<1e-9); CHECK(ctx.flat_order_map.active_count()==0); }
        if(variant==3){ OrderResult e=R(xk,OrderStatus::ERROR,false,0,0,0,902,false); e.error_msg.set("NET_UNKNOWN",11);
            dispatch_order_result(ctx,e); CHECK(ctx.position.qty==10.0); CHECK(ctx.flat_order_map.active_count()==1); // UNKNOWN: reconcile bekler
            dispatch_order_result(ctx,R(xk,OrderStatus::FILLED,false,10.0,0.0,101.0,902,false)); CHECK(!ctx.position.is_open()); }
    }
    }
#endif
    std::printf("\nSONUC: %s (fails=%d)\n", fails?"BASARISIZ":"GECTI", fails);
    return fails?1:0;
}
