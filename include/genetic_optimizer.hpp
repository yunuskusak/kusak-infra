// ============================================================================
// KUSAK V9 — RESEARCH FRAMEWORK
// genetic_optimizer.hpp — Genetik/Evrimsel Parametre Optimizasyonu
//
// Rapor §7: Grid ve Bayes optimizasyonuna ek olarak evrimsel algoritma.
// Genetic approach ile parametre uzayı geniş çapta taranır; en iyi
// sonuç verenler bir sonraki nesle aktarılır.
//
// Algoritma:
//   1. Başlangıç popülasyonu: N aday parametre seti (rastgele)
//   2. Her nesilde: backtest çalıştır, fitness hesapla
//   3. Seçilim (tournament/roulette), Çaprazlama, Mutasyon
//   4. Elitizm: En iyi M aday değişmeden sonraki nesle geçer
//   5. Durdurma: max_generations veya fitness_stall_generations
//
// Thread safety: her birey ayrı Backtester instance kullanır → thread-safe.
// Paralel fitness hesaplaması için std::thread pool kullanılır.
//
// Referans: Zenbot darwin.js, DEAP (Python) yaklaşımı C++ portu
// ============================================================================

#pragma once

#include "research_types.hpp"
#include <vector>
#include <functional>
#include <random>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <future>
#include <string>
#include <sstream>
#include <chrono>

namespace kusak::research {

// ============================================================================
// §1  Arama Uzayı Tanımı
//     Her parametre için [min, max] aralığı tanımlanır.
//     GeneticOptimizer bu aralıkları kullanarak bireyleri üretir.
// ============================================================================

struct GeneRange {
    double min_val;
    double max_val;
    std::string name;  // debug/log için
};

// ResearchParams alanlarına karşılık gelen arama uzayı
struct GeneticSearchSpace {
    GeneRange vpin_threshold        { 0.40, 0.90, "vpin_threshold"        };
    GeneRange min_edge              { 0.0002, 0.002, "min_edge"           };
    GeneRange imbalance_threshold   { 0.10, 0.50, "imbalance_threshold"   };
    GeneRange fill_prob_threshold   { 0.01, 0.10, "fill_prob_threshold"   };
    GeneRange sniper_score_mult     { 1.0, 3.0,  "sniper_score_mult"      };
    GeneRange max_depth             { 500.0, 5000.0, "max_depth"          };
    GeneRange edge_alpha            { 0.10, 0.90, "edge_alpha"            };
    GeneRange edge_beta             { 0.05, 0.50, "edge_beta"             };
    GeneRange trailing_activation   { 0.0, 30.0, "trailing_activation_bps"};
    GeneRange trailing_callback     { 2.0, 20.0, "trailing_callback_bps"  };
};

// ============================================================================
// §2  Genetik Optimizasyon Konfigürasyonu
// ============================================================================

struct GeneticConfig {
    int    population_size          = 50;    // nesil başına birey sayısı
    int    max_generations          = 30;    // maksimum nesil
    double crossover_rate           = 0.70;  // çaprazlama olasılığı
    double mutation_rate            = 0.15;  // mutasyon olasılığı
    double mutation_std_factor      = 0.15;  // mutasyon stddev = aralık × bu faktör
    int    elitism_count            = 3;     // değişmeden geçen en iyi N birey
    int    tournament_size          = 3;     // turnuva seçimi boyutu
    int    stall_generations        = 8;     // N nesil iyileşme yoksa dur
    int    n_workers                = 4;     // paralel iş parçacığı sayısı
    bool   verbose                  = true;  // nesil bazlı log
    bool   use_sharpe_fitness       = true;  // true=Sharpe, false=net_pnl

    GeneticSearchSpace search_space;
};

// ============================================================================
// §3  Birey (Individual) — parametre seti + fitness
// ============================================================================

struct Individual {
    ResearchParams params;
    double         fitness = -1e18;
    bool           evaluated = false;

    bool operator<(const Individual& o) const noexcept {
        return fitness > o.fitness;  // büyük fitness önce (descending sort)
    }
};

// ============================================================================
// §4  GeneticOptimizer
// ============================================================================

class GeneticOptimizer {
public:
    using FitnessFn = std::function<BacktestResult(const ResearchParams&)>;

    explicit GeneticOptimizer(GeneticConfig cfg = {})
        : cfg_(std::move(cfg))
        , rng_(std::random_device{}())
    {}

    // ── Ana optimizasyon döngüsü ──────────────────────────────────────────
    Individual run(const FitnessFn& fitness_fn) {
        auto t_start = std::chrono::steady_clock::now();

        // İlk popülasyonu rastgele üret
        std::vector<Individual> population = initialize();

        // Fitness hesapla (ilk nesil)
        evaluate_population(population, fitness_fn);
        std::sort(population.begin(), population.end());

        double best_ever   = population[0].fitness;
        int    stall_count = 0;
        Individual best_individual = population[0];

        if (cfg_.verbose) {
            print_header();
            print_generation(0, population, t_start);
        }

        for (int gen = 1; gen <= cfg_.max_generations; ++gen) {
            // Yeni nesil üret
            std::vector<Individual> offspring = breed(population);

            // Fitness değerlendir
            evaluate_population(offspring, fitness_fn);

            // Elitizm: en iyi cfg_.elitism_count bireyler korunur
            std::sort(population.begin(), population.end());
            for (int e = 0; e < cfg_.elitism_count && e < (int)population.size(); ++e)
                offspring.push_back(population[e]);

            // Yeni nesli sırala, fazlasını kes
            std::sort(offspring.begin(), offspring.end());
            if ((int)offspring.size() > cfg_.population_size)
                offspring.resize(cfg_.population_size);

            population = std::move(offspring);

            // En iyi güncelle
            if (population[0].fitness > best_ever) {
                best_ever      = population[0].fitness;
                best_individual= population[0];
                stall_count    = 0;
            } else {
                stall_count++;
            }

            if (cfg_.verbose)
                print_generation(gen, population, t_start);

            // Durma kriteri
            if (stall_count >= cfg_.stall_generations) {
                if (cfg_.verbose)
                    std::cout << "[GeneticOpt] " << stall_count
                              << " nesil iyileşme yok — duruldu.\n";
                break;
            }
        }

        if (cfg_.verbose) print_best(best_individual);
        return best_individual;
    }

    const GeneticConfig& config() const noexcept { return cfg_; }

private:
    // ========================================================================
    // §4.1  Popülasyon Başlatma — rastgele birey üret
    // ========================================================================

    std::vector<Individual> initialize() {
        std::vector<Individual> pop;
        pop.reserve(cfg_.population_size);
        const auto& ss = cfg_.search_space;

        for (int i = 0; i < cfg_.population_size; ++i) {
            Individual ind;
            ind.params.vpin_threshold       = random_in(ss.vpin_threshold.min_val,       ss.vpin_threshold.max_val);
            ind.params.min_edge             = random_in(ss.min_edge.min_val,             ss.min_edge.max_val);
            ind.params.imbalance_threshold  = random_in(ss.imbalance_threshold.min_val,  ss.imbalance_threshold.max_val);
            ind.params.fill_prob_threshold  = random_in(ss.fill_prob_threshold.min_val,  ss.fill_prob_threshold.max_val);
            ind.params.sniper_score_mult    = random_in(ss.sniper_score_mult.min_val,    ss.sniper_score_mult.max_val);
            ind.params.max_depth            = random_in(ss.max_depth.min_val,            ss.max_depth.max_val);
            ind.params.edge_alpha           = random_in(ss.edge_alpha.min_val,           ss.edge_alpha.max_val);
            ind.params.edge_beta            = random_in(ss.edge_beta.min_val,            ss.edge_beta.max_val);
            ind.params.trailing_activation_bps = random_in(ss.trailing_activation.min_val, ss.trailing_activation.max_val);
            ind.params.trailing_callback_bps   = random_in(ss.trailing_callback.min_val,   ss.trailing_callback.max_val);
            pop.push_back(ind);
        }
        return pop;
    }

    // ========================================================================
    // §4.2  Paralel Fitness Değerlendirmesi
    //
    // FIX: Önceki kodda future'lar wait() ile bekleniyordu.
    // Sorun: wait() exception'ı yakalamaz; thread içinde fırlatılan bir
    // exception future'da depolanır ve wait() döndükten sonra get()
    // çağrıldığında yeniden fırlatılır. Ancak eski kodda get() hiç
    // çağrılmıyordu — yani exception tamamen yutulup fitness güncellenmeden
    // bireyin geçersiz (fitness=-1e18 initial değerle) kalmasına ya da
    // future destructor'ında std::terminate çağrılmasına yol açabilirdi.
    //
    // Çözüm: future<BacktestResult> kullan; her future için get() çağır
    // ve exception'ı try-catch ile yakala. Hatalı bireyin fitness'i -1e18
    // olarak işaretlenir, popülasyon devam eder.
    //
    // İmplementasyon notu: future'ları pop indeksiyle eşleştirmek için
    // TaggedFuture yapısı kullanılıyor; bu sayede exception durumunda
    // hangi bireyin hatalı olduğu belirlenip işaretlenebiliyor.
    // ========================================================================

    void evaluate_population(std::vector<Individual>& pop, const FitnessFn& fn) {
        int n       = static_cast<int>(pop.size());
        int workers = std::min(cfg_.n_workers, n);

        // future'ı hangi pop indeksine ait olduğuyla birlikte tut
        struct TaggedFuture {
            std::future<BacktestResult> fut;
            int                         pop_idx;
        };

        std::vector<TaggedFuture> futures;
        futures.reserve(n);

        // Aktif görev penceresini workers ile sınırlayan yardımcı lambda
        // FIX: wait() → get() + try-catch
        auto drain_one = [&]() {
            int idx = futures.front().pop_idx;
            try {
                BacktestResult res      = futures.front().fut.get();
                pop[idx].fitness        = compute_fitness(res);
                pop[idx].evaluated      = true;
            } catch (const std::exception& e) {
                std::cerr << "[GeneticOpt] Fitness hesaplama hatası"
                          << " (birey " << idx << "): " << e.what() << "\n";
                pop[idx].fitness   = -1e18;
                pop[idx].evaluated = true;
            } catch (...) {
                std::cerr << "[GeneticOpt] Fitness hesaplama hatası"
                          << " (birey " << idx << "): bilinmeyen istisna\n";
                pop[idx].fitness   = -1e18;
                pop[idx].evaluated = true;
            }
            futures.erase(futures.begin());
        };

        for (int i = 0; i < n; ++i) {
            if (pop[i].evaluated) continue; // elitler yeniden değerlendirilmez

            futures.push_back({
                std::async(std::launch::async, [&fn, params = pop[i].params]() {
                    return fn(params);
                }),
                i
            });

            // Pencere dolu → en eski görevi bekle (get + exception handling)
            if ((int)futures.size() >= workers) {
                drain_one();
            }
        }

        // Kalan tüm future'ları tüket
        while (!futures.empty()) {
            drain_one();
        }
    }

    // ========================================================================
    // §4.3  Üreme (Breeding) — Seçim + Çaprazlama + Mutasyon
    // ========================================================================

    std::vector<Individual> breed(const std::vector<Individual>& pop) {
        std::vector<Individual> offspring;
        offspring.reserve(cfg_.population_size);

        while ((int)offspring.size() < cfg_.population_size) {
            // Turnuva seçimi: 2 ebeveyn
            const Individual& p1 = tournament_select(pop);
            const Individual& p2 = tournament_select(pop);

            // Çaprazlama
            std::uniform_real_distribution<double> prob(0.0, 1.0);
            if (prob(rng_) < cfg_.crossover_rate) {
                auto [c1, c2] = crossover(p1, p2);
                mutate(c1);
                mutate(c2);
                c1.evaluated = false;
                c2.evaluated = false;
                offspring.push_back(c1);
                if ((int)offspring.size() < cfg_.population_size) {
                    offspring.push_back(c2);
                }
            } else {
                Individual child = p1;
                mutate(child);
                child.evaluated = false;
                offspring.push_back(child);
            }
        }
        return offspring;
    }

    // Turnuva seçimi
    const Individual& tournament_select(const std::vector<Individual>& pop) {
        std::uniform_int_distribution<int> idx(0, (int)pop.size() - 1);
        const Individual* best = &pop[idx(rng_)];
        for (int t = 1; t < cfg_.tournament_size; ++t) {
            const Individual* candidate = &pop[idx(rng_)];
            if (candidate->fitness > best->fitness)
                best = candidate;
        }
        return *best;
    }

    // Uniform çaprazlama: her gen için rastgele ebeveyn seç
    std::pair<Individual, Individual> crossover(const Individual& p1,
                                                 const Individual& p2) {
        Individual c1 = p1, c2 = p2;
        std::uniform_real_distribution<double> coin(0.0, 1.0);

        auto swap_gene = [&](double& g1, double& g2) {
            if (coin(rng_) < 0.5) std::swap(g1, g2);
        };

        swap_gene(c1.params.vpin_threshold,          c2.params.vpin_threshold);
        swap_gene(c1.params.min_edge,                c2.params.min_edge);
        swap_gene(c1.params.imbalance_threshold,     c2.params.imbalance_threshold);
        swap_gene(c1.params.fill_prob_threshold,     c2.params.fill_prob_threshold);
        swap_gene(c1.params.sniper_score_mult,       c2.params.sniper_score_mult);
        swap_gene(c1.params.max_depth,               c2.params.max_depth);
        swap_gene(c1.params.edge_alpha,              c2.params.edge_alpha);
        swap_gene(c1.params.edge_beta,               c2.params.edge_beta);
        swap_gene(c1.params.trailing_activation_bps, c2.params.trailing_activation_bps);
        swap_gene(c1.params.trailing_callback_bps,   c2.params.trailing_callback_bps);

        return {c1, c2};
    }

    // Gaussian mutasyon
    void mutate(Individual& ind) {
        const auto& ss = cfg_.search_space;
        std::uniform_real_distribution<double> prob(0.0, 1.0);

        auto maybe_mutate = [&](double& val, const GeneRange& range) {
            if (prob(rng_) < cfg_.mutation_rate) {
                double std = (range.max_val - range.min_val) * cfg_.mutation_std_factor;
                std::normal_distribution<double> gauss(val, std);
                val = std::clamp(gauss(rng_), range.min_val, range.max_val);
            }
        };

        maybe_mutate(ind.params.vpin_threshold,          ss.vpin_threshold);
        maybe_mutate(ind.params.min_edge,                ss.min_edge);
        maybe_mutate(ind.params.imbalance_threshold,     ss.imbalance_threshold);
        maybe_mutate(ind.params.fill_prob_threshold,     ss.fill_prob_threshold);
        maybe_mutate(ind.params.sniper_score_mult,       ss.sniper_score_mult);
        maybe_mutate(ind.params.max_depth,               ss.max_depth);
        maybe_mutate(ind.params.edge_alpha,              ss.edge_alpha);
        maybe_mutate(ind.params.edge_beta,               ss.edge_beta);
        maybe_mutate(ind.params.trailing_activation_bps, ss.trailing_activation);
        maybe_mutate(ind.params.trailing_callback_bps,   ss.trailing_callback);
    }

    // ========================================================================
    // §4.4  Fitness fonksiyonu
    // ========================================================================

    double compute_fitness(const BacktestResult& r) const noexcept {
        if (!r.valid || r.total_trades < 10.0) return -1e18;
        if (cfg_.use_sharpe_fitness) {
            // Sharpe ağırlıklı composite
            return r.sharpe_ratio * 0.5
                 + r.net_pnl      * 0.3
                 - r.max_drawdown * 0.2;
        } else {
            // Net PnL odaklı
            return r.net_pnl * 0.6
                 + r.win_rate * 0.2
                 - r.max_drawdown * 0.2;
        }
    }

    // ========================================================================
    // §4.5  Yardımcılar
    // ========================================================================

    double random_in(double lo, double hi) {
        std::uniform_real_distribution<double> d(lo, hi);
        return d(rng_);
    }

    using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;

    void print_header() const {
        std::cout << "\n[GeneticOpt] Başlatılıyor\n"
                  << "  Popülasyon : " << cfg_.population_size << "\n"
                  << "  Max Nesil  : " << cfg_.max_generations << "\n"
                  << "  İş parçacığı: " << cfg_.n_workers << "\n"
                  << "  Çaprazlama : " << cfg_.crossover_rate << "\n"
                  << "  Mutasyon   : " << cfg_.mutation_rate << "\n\n"
                  << std::left << std::setw(6) << "Nesil"
                  << std::setw(14) << "En İyi Fit."
                  << std::setw(14) << "Ort. Fit."
                  << std::setw(10) << "Süre (s)"
                  << "\n" << std::string(50, '-') << "\n";
    }

    void print_generation(int gen, const std::vector<Individual>& pop,
                           const TimePoint& t0) const {
        double sum = 0.0, n = 0.0;
        for (const auto& ind : pop) {
            if (ind.fitness > -1e17) { sum += ind.fitness; n += 1.0; }
        }
        double avg = (n > 0) ? sum / n : 0.0;
        double el  = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();

        std::cout << std::left << std::setw(6) << gen
                  << std::setw(14) << std::fixed << std::setprecision(6)
                  << pop[0].fitness
                  << std::setw(14) << avg
                  << std::setw(10) << std::setprecision(1) << el
                  << "\n";
    }

    void print_best(const Individual& best) const {
        const auto& p = best.params;
        std::cout << "\n[GeneticOpt] === En İyi Birey ===\n"
                  << "  Fitness              : " << std::fixed << std::setprecision(6) << best.fitness << "\n"
                  << "  vpin_threshold       : " << p.vpin_threshold << "\n"
                  << "  min_edge             : " << p.min_edge << "\n"
                  << "  imbalance_threshold  : " << p.imbalance_threshold << "\n"
                  << "  sniper_score_mult    : " << p.sniper_score_mult << "\n"
                  << "  edge_alpha           : " << p.edge_alpha << "\n"
                  << "  edge_beta            : " << p.edge_beta << "\n"
                  << "  trailing_activation  : " << p.trailing_activation_bps << " bps\n"
                  << "  trailing_callback    : " << p.trailing_callback_bps   << " bps\n";
    }

    GeneticConfig       cfg_;
    std::mt19937_64     rng_;
};

} // namespace kusak::research
