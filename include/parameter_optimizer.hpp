#pragma once
#ifndef KUSAK_PARAMETER_OPTIMIZER_HPP
#define KUSAK_PARAMETER_OPTIMIZER_HPP

// Fixed:
// 1. ParameterOptimizerConfig lifted outside class
// 2. explicit ParameterOptimizer(ObjectiveFn, Config) — canonical ctor
// 3. obj_fn_ member type = ObjectiveFn (research_types.hpp)

#include "core_types.hpp"
#include "research_types.hpp"
#include "backtester.hpp"

#include <vector>
#include <string>
#include <functional>
#include <random>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <limits>
#include <atomic>
#include <mutex>
#include <thread>
#include <future>
#include <climits>

namespace kusak::research {

enum class ParamType : uint8_t { CONTINUOUS = 0, INTEGER = 1, CATEGORICAL = 2 };

struct ParamDef {
    std::string name;
    ParamType   type    = ParamType::CONTINUOUS;
    double      min_val = 0.0;
    double      max_val = 1.0;
    double      step    = 0.0;
    int         n_choices = 0;
};

using ParamSpace = std::vector<ParamDef>;

inline ResearchParams params_from_vec(const std::vector<double>& v) noexcept {
    ResearchParams p;
    if (v.size() > 0) p.entry_threshold  = std::clamp(v[0], 1e-4, 0.05);
    if (v.size() > 1) p.exit_threshold   = std::clamp(v[1], 1e-4, 0.05);
    if (v.size() > 2) p.stop_loss        = std::clamp(v[2], 1e-3, 0.05);
    if (v.size() > 3) p.take_profit      = std::clamp(v[3], 1e-3, 0.10);
    if (v.size() > 4) p.position_size    = std::clamp(v[4], 0.01, 10.0);
    if (v.size() > 5) p.vpin_buckets     = static_cast<int>(std::clamp(v[5], 10.0, 200.0));
    if (v.size() > 6) p.vpin_threshold   = std::clamp(v[6], 0.30, 0.95);
    if (v.size() > 7) p.imbalance_thresh = std::clamp(v[7], 0.10, 0.90);
    if (v.size() > 8) p.depth_levels     = static_cast<int>(std::clamp(v[8], 1.0, 20.0));
    return p;
}

inline std::vector<double> vec_from_params(const ResearchParams& p) noexcept {
    return { p.entry_threshold, p.exit_threshold, p.stop_loss, p.take_profit,
             p.position_size, static_cast<double>(p.vpin_buckets),
             p.vpin_threshold, p.imbalance_thresh,
             static_cast<double>(p.depth_levels) };
}

inline std::pair<std::vector<double>, std::vector<double>> default_bounds() noexcept {
    return { {1e-4,1e-4,1e-3,1e-3,0.01,10,0.30,0.10,1},
             {0.05,0.05,0.05,0.10,10.0,200,0.95,0.90,20} };
}

struct OptimizationResult {
    ResearchParams best_params;
    BacktestResult best_backtest;
    double best_score = -std::numeric_limits<double>::infinity();
    int    n_evals = 0;
};

inline double score(const BacktestResult& r) noexcept {
    if (r.total_trades < 5) return -1e9;
    return r.sharpe - 0.1 * r.max_drawdown;
}

// Config outside class: avoids CWG 2346 default-arg on incomplete type
struct ParameterOptimizerConfig {
    int    n_random_start = 64;
    int    n_bayesian     = 128;
    int    n_threads      = 0;      // 0 = hardware_concurrency
    int    seed           = 42;
    double exploit_ratio  = 0.7;
    bool   use_genetic    = false;
    int    genetic_pop    = 64;
    int    genetic_gen    = 50;
};

class ParameterOptimizer {
public:
    using Config = ParameterOptimizerConfig;

    // Canonical ctor — hata logundaki: explicit ParameterOptimizer(ObjectiveFn, Config)
    explicit ParameterOptimizer(ObjectiveFn obj_fn,
                                Config      cfg = Config{})
        : obj_fn_(std::move(obj_fn))
        , cfg_(cfg)
        , rng_(static_cast<uint_fast32_t>(cfg.seed))
    {
        n_threads_ = cfg.n_threads > 0
            ? cfg.n_threads
            : static_cast<int>(std::thread::hardware_concurrency());
        if (n_threads_ < 1) n_threads_ = 1;
    }

    ParameterOptimizer(const ParameterOptimizer&)            = delete;
    ParameterOptimizer& operator=(const ParameterOptimizer&) = delete;
    ParameterOptimizer(ParameterOptimizer&&)                 = delete;
    ParameterOptimizer& operator=(ParameterOptimizer&&)      = delete;

    OptimizationResult run(const ParamSpace& space = ParamSpace{}) {
        auto [lo, hi] = space.empty() ? default_bounds() : bounds_from(space);
        OptimizationResult res;
        phase1_random(lo, hi, res);
        phase2_bayesian(lo, hi, res);
        if (cfg_.use_genetic) phase3_genetic(lo, hi, res);
        return res;
    }

    int n_evals() const noexcept { return n_evals_.load(std::memory_order_relaxed); }

private:
    ObjectiveFn      obj_fn_;   // std::function<BacktestResult(const ResearchParams&)>
    Config           cfg_;
    std::mt19937     rng_;
    int              n_threads_;
    std::atomic<int> n_evals_{0};
    std::mutex       mtx_;

    static std::pair<std::vector<double>, std::vector<double>>
    bounds_from(const ParamSpace& s) noexcept {
        std::vector<double> lo, hi;
        lo.reserve(s.size()); hi.reserve(s.size());
        for (const auto& d : s) { lo.push_back(d.min_val); hi.push_back(d.max_val); }
        return {lo, hi};
    }

    std::vector<double> rand_cand(const std::vector<double>& lo,
                                   const std::vector<double>& hi) noexcept {
        std::vector<double> v(lo.size());
        for (std::size_t i = 0; i < lo.size(); ++i)
            v[i] = std::uniform_real_distribution<double>{lo[i], hi[i]}(rng_);
        return v;
    }

    double eval(const std::vector<double>& v, OptimizationResult& res) {
        const ResearchParams p  = params_from_vec(v);
        const BacktestResult bt = obj_fn_(p);
        n_evals_.fetch_add(1, std::memory_order_relaxed);
        const double s = score(bt);
        std::lock_guard<std::mutex> lk(mtx_);
        if (s > res.best_score) { res.best_score = s; res.best_params = p; res.best_backtest = bt; }
        ++res.n_evals;
        return s;
    }

    void phase1_random(const std::vector<double>& lo,
                       const std::vector<double>& hi,
                       OptimizationResult& res) {
        std::vector<std::vector<double>> cands;
        cands.reserve(cfg_.n_random_start);
        for (int i = 0; i < cfg_.n_random_start; ++i) cands.push_back(rand_cand(lo, hi));
        if (n_threads_ == 1) { for (const auto& c : cands) eval(c, res); return; }
        std::vector<std::future<void>> futs;
        futs.reserve(cands.size());
        for (std::size_t i = 0; i < cands.size(); ++i)
            futs.push_back(std::async(std::launch::async,
                [this, &cands, &res, i]{ eval(cands[i], res); }));
        for (auto& f : futs) f.get();
    }

    void phase2_bayesian(const std::vector<double>& lo,
                         const std::vector<double>& hi,
                         OptimizationResult& res) {
        const std::size_t dim = lo.size();
        std::vector<double> best_v = vec_from_params(res.best_params);
        std::uniform_real_distribution<double> coin{0.0, 1.0};
        for (int iter = 0; iter < cfg_.n_bayesian; ++iter) {
            std::vector<double> cand(dim);
            if (coin(rng_) < cfg_.exploit_ratio) {
                for (std::size_t i = 0; i < dim; ++i) {
                    const double sigma = (hi[i] - lo[i]) * 0.05 * std::pow(0.95, iter);
                    cand[i] = std::clamp(
                        std::normal_distribution<double>{best_v[i], sigma}(rng_),
                        lo[i], hi[i]);
                }
            } else { cand = rand_cand(lo, hi); }
            if (eval(cand, res) >= res.best_score - 1e-9)
                best_v = vec_from_params(res.best_params);
        }
    }

    void phase3_genetic(const std::vector<double>& lo,
                        const std::vector<double>& hi,
                        OptimizationResult& res) {
        const std::size_t dim = lo.size();
        const int pop = cfg_.genetic_pop;
        std::vector<std::pair<double, std::vector<double>>> pop_v;
        pop_v.reserve(pop);
        pop_v.emplace_back(res.best_score, vec_from_params(res.best_params));
        for (int i = 1; i < pop; ++i) pop_v.emplace_back(-1e18, rand_cand(lo, hi));
        std::uniform_real_distribution<double> coin{0.0, 1.0};
        std::uniform_int_distribution<int>     pick{0, pop - 1};
        for (int g = 0; g < cfg_.genetic_gen; ++g) {
            for (auto& [s, v] : pop_v) if (s < -1e17) s = eval(v, res);
            std::sort(pop_v.begin(), pop_v.end(), [](const auto& a, const auto& b){ return a.first > b.first; });
            const int elite = std::max(2, pop / 4);
            std::vector<std::pair<double, std::vector<double>>> next;
            next.reserve(pop);
            for (int i = 0; i < elite; ++i) next.push_back(pop_v[i]);
            while (static_cast<int>(next.size()) < pop) {
                const auto& p1 = pop_v[pick(rng_) % elite].second;
                const auto& p2 = pop_v[pick(rng_) % elite].second;
                std::vector<double> child(dim);
                for (std::size_t i = 0; i < dim; ++i) {
                    child[i] = (coin(rng_) < 0.5) ? p1[i] : p2[i];
                    if (coin(rng_) < 0.1)
                        child[i] = std::clamp(
                            std::normal_distribution<double>{child[i], (hi[i]-lo[i])*0.05}(rng_),
                            lo[i], hi[i]);
                }
                next.emplace_back(-1e18, std::move(child));
            }
            pop_v = std::move(next);
        }
    }
};

} // namespace kusak::research
#endif // KUSAK_PARAMETER_OPTIMIZER_HPP
