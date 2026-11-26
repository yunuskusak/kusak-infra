#pragma once
#ifndef KUSAK_WALK_FORWARD_OPTIMIZER_HPP
#define KUSAK_WALK_FORWARD_OPTIMIZER_HPP

#include "core_types.hpp"
#include "research_types.hpp"
#include "backtester.hpp"

#include <span>
#include <vector>
#include <climits>
#include <algorithm>

namespace kusak::research {

struct WFOConfig {
    std::size_t in_sample_ticks  = 100'000;
    std::size_t out_sample_ticks =  25'000;
    std::size_t step_ticks       =  25'000;
    int         max_folds        = INT_MAX;
};

struct WFOFoldResult {
    std::size_t    in_start, in_end, out_start, out_end;
    ResearchParams best_params;
    BacktestResult in_result;
    BacktestResult out_result;
};

class WalkForwardOptimizer {
public:
    // (A) span
    WalkForwardOptimizer(std::span<const TickRecord> all_ticks,
                         WFOObjectiveFn               obj,
                         WFOConfig                    cfg = WFOConfig{}) noexcept
        : ticks_(all_ticks), obj_(std::move(obj)), cfg_(cfg) {}

    // (B) ptr+size  ← research_main.cpp: mv.data(), mv.size(), obj
    WalkForwardOptimizer(const TickRecord* data, std::size_t count,
                         WFOObjectiveFn    obj,
                         WFOConfig         cfg = WFOConfig{}) noexcept
        : WalkForwardOptimizer(std::span<const TickRecord>{data, count},
                               std::move(obj), cfg) {}

    // (C) vector&
    WalkForwardOptimizer(const std::vector<TickRecord>& v,
                         WFOObjectiveFn                  obj,
                         WFOConfig                       cfg = WFOConfig{}) noexcept
        : WalkForwardOptimizer(std::span<const TickRecord>{v},
                               std::move(obj), cfg) {}

    std::vector<WFOFoldResult> run() {
        std::vector<WFOFoldResult> results;
        const std::size_t n   = ticks_.size();
        const TickRecord* ptr = ticks_.data();
        std::size_t in_s = 0;
        int fold = 0;

        while (fold < cfg_.max_folds) {
            const std::size_t in_e  = in_s + cfg_.in_sample_ticks;
            const std::size_t out_s = in_e;
            const std::size_t out_e = std::min(out_s + cfg_.out_sample_ticks, n);
            if (in_e > n || out_s >= n) break;

            const std::size_t in_len  = in_e  - in_s;
            const std::size_t out_len = out_e - out_s;

            ResearchParams best{};
            const BacktestResult in_r  = obj_(best, ptr + in_s, in_len,  ptr + in_s,  in_len);
            const BacktestResult out_r = obj_(best, ptr + in_s, in_len,  ptr + out_s, out_len);

            results.push_back({in_s, in_e, out_s, out_e, best, in_r, out_r});
            in_s += cfg_.step_ticks;
            ++fold;
        }
        return results;
    }

    int max_folds_possible() const noexcept {
        const std::size_t n = ticks_.size();
        if (n < cfg_.in_sample_ticks + cfg_.out_sample_ticks) return 0;
        return static_cast<int>(
            (n - cfg_.in_sample_ticks - cfg_.out_sample_ticks) / cfg_.step_ticks + 1);
    }

private:
    std::span<const TickRecord> ticks_;
    WFOObjectiveFn              obj_;
    WFOConfig                   cfg_;
};

} // namespace kusak::research
#endif // KUSAK_WALK_FORWARD_OPTIMIZER_HPP
