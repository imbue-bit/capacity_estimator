#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <iomanip>
#include <random>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <stdexcept>
#include <chrono>

#define LOG_STEP(msg)  std::cout << "\n#### step: " << msg << "\n"
#define LOG_INFO(msg)  std::cout << "#### info: " << msg << "\n"
#define LOG_WARN(msg)  std::cout << "#### warn: " << msg << "\n"
#define LOG_ERROR(msg) std::cerr << "#### err:  " << msg << "\n"

namespace quant {

inline void show_progress(size_t current, size_t total, const std::string& prefix = "") {
    if (total == 0) return;
    const int width = 50;
    float progress = std::clamp(static_cast<float>(current) / total, 0.0f, 1.0f);
    int pos = static_cast<int>(width * progress);
    std::cout << "\r" << prefix << " [";
    for (int i = 0; i < width; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << std::fixed << std::setprecision(2) << (progress * 100.0) << "%" << std::flush;
    if (current >= total) std::cout << "\n";
}

struct run_config {
    std::string l2_path = "";
    std::string trades_path = "";
    
    double daily_vol = 0.015;
    double daily_adv = 1e10;
    
    double spread_th = 0.5;
    double imb_th = 0.33;
    
    double lr = 0.1;
    double lr_decay = 0.99;
    double lr_min = 0.001;
    double gamma = 0.9;
    double epsilon = 0.2;
    double epsilon_decay = 0.95;
    double epsilon_min = 0.01;
    double reward_clip = 100.0;
    int episodes = 10;
    int lookahead_ticks = 10;
    
    double aum_min = 1e7;
    double aum_max = 1e10;
    double aum_step = 1e7;

    double impact_exponent = 0.5;
    double max_participation_rate = 0.05;
    
    double permanent_impact_coeff = 0.15;
    double baseline_slip_bps = 5.0;
};

struct l2_tick {
    uint64_t timestamp;
    double last_price;
    double bid1_p, bid1_v;
    double ask1_p, ask1_v;
    double volume;
    double turnover;
};

struct trade_signal {
    uint64_t timestamp;
    int direction;
    double target_weight; 
    double alpha_bps; 
};

inline bool is_valid_double(double val) {
    return !std::isnan(val) && !std::isinf(val);
}

inline bool parse_l2_line_fast(const char* ptr, l2_tick& tick) {
    char* end;
    tick.timestamp = std::strtoull(ptr, &end, 10); if (ptr == end) return false; ptr = end + 1;
    tick.last_price = std::strtod(ptr, &end); if (ptr == end) return false; ptr = end + 1;
    tick.bid1_p = std::strtod(ptr, &end); if (ptr == end) return false; ptr = end + 1;
    tick.bid1_v = std::strtod(ptr, &end); if (ptr == end) return false; ptr = end + 1;
    tick.ask1_p = std::strtod(ptr, &end); if (ptr == end) return false; ptr = end + 1;
    tick.ask1_v = std::strtod(ptr, &end); if (ptr == end) return false; ptr = end + 1;
    tick.volume = std::strtod(ptr, &end); if (ptr == end) return false; ptr = end + 1;
    tick.turnover = std::strtod(ptr, &end);
    return true;
}

inline bool parse_trade_line_fast(const char* ptr, trade_signal& sig) {
    char* end;
    sig.timestamp = std::strtoull(ptr, &end, 10); if (ptr == end) return false; ptr = end + 1;
    sig.direction = std::strtol(ptr, &end, 10); if (ptr == end) return false; ptr = end + 1;
    sig.target_weight = std::strtod(ptr, &end); if (ptr == end) return false; ptr = end + 1;
    sig.alpha_bps = std::strtod(ptr, &end);
    return true;
}

class orderbook_discretizer {
private:
    double spread_th_;
    double imb_th_;

public:
    static constexpr int num_states = 6;
    static constexpr int num_actions = 3; 

    explicit orderbook_discretizer(const run_config& cfg) 
        : spread_th_(cfg.spread_th), imb_th_(cfg.imb_th) {}

    [[nodiscard]] int get_state(const l2_tick& tick) const noexcept {
        double spread_bps = (tick.ask1_p - tick.bid1_p) / ((tick.ask1_p + tick.bid1_p) * 0.5) * 10000.0;
        double imbalance = (tick.bid1_v - tick.ask1_v) / (tick.bid1_v + tick.ask1_v + 1e-9);

        int spread_idx = (spread_bps > spread_th_) ? 1 : 0; 
        
        int imb_idx = 1; 
        if (imbalance > imb_th_) imb_idx = 2; 
        else if (imbalance < -imb_th_) imb_idx = 0; 

        return spread_idx * 3 + imb_idx;
    }

    [[nodiscard]] static std::string state_to_string(int state) {
        static const std::vector<std::string> names = {
            "narrow_ask_heavy", "narrow_balanced", "narrow_bid_heavy",
            "wide_ask_heavy", "wide_balanced", "wide_bid_heavy"
        };
        return names[state];
    }
};

class data_loader {
public:
    static std::vector<l2_tick> load_l2_csv(const std::string& filepath) {
        LOG_STEP("loading l2 ticks: " + filepath);
        auto start_time = std::chrono::high_resolution_clock::now();
        
        std::vector<l2_tick> ticks;
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) throw std::runtime_error("failed to open l2 csv: " + filepath);

        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        ticks.reserve(file_size / 50);

        std::string line;
        std::getline(file, line);
        size_t bytes_read = line.size() + 1;

        double total_spread = 0.0;
        size_t skipped_rows = 0;
        size_t processed_rows = 0;

        while (std::getline(file, line)) {
            bytes_read += line.size() + 1;
            processed_rows++;
            
            if (processed_rows % 50000 == 0) {
                show_progress(bytes_read, file_size, "  - parsing l2 data ");
            }

            if (line.empty() || line.find_first_not_of("\r\n\t ") == std::string::npos) continue;

            l2_tick tick;
            if (!parse_l2_line_fast(line.c_str(), tick) ||
                !is_valid_double(tick.last_price) || !is_valid_double(tick.bid1_p) || 
                !is_valid_double(tick.ask1_p) || tick.bid1_p <= 0 || tick.ask1_p <= 0 || 
                tick.ask1_p < tick.bid1_p) {
                skipped_rows++;
                continue;
            }

            total_spread += (tick.ask1_p - tick.bid1_p);
            ticks.push_back(tick);
        }
        show_progress(file_size, file_size, "  - parsing l2 data ");
        
        if (skipped_rows > 0) {
            LOG_WARN("skipped " + std::to_string(skipped_rows) + " malformed or invalid rows in l2 data");
        }

        std::sort(ticks.begin(), ticks.end(), [](const l2_tick& a, const l2_tick& b) {
            return a.timestamp < b.timestamp;
        });

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end_time - start_time;

        if (!ticks.empty()) {
            LOG_INFO("l2 load complete in " + std::to_string(elapsed.count()) + "s");
            std::cout << "  - valid records:  " << ticks.size() << "\n";
            std::cout << "  - avg spread:     " << std::fixed << std::setprecision(4) << (total_spread / ticks.size()) << "\n";
        }
        return ticks;
    }

    static std::vector<trade_signal> load_trades_csv(const std::string& filepath) {
        LOG_STEP("loading trade logs: " + filepath);
        auto start_time = std::chrono::high_resolution_clock::now();
        
        std::vector<trade_signal> signals;
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) throw std::runtime_error("failed to open trades csv: " + filepath);

        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::string line;
        std::getline(file, line);
        size_t bytes_read = line.size() + 1;

        double avg_alpha = 0.0;
        size_t skipped_rows = 0;
        size_t processed_rows = 0;

        while (std::getline(file, line)) {
            bytes_read += line.size() + 1;
            processed_rows++;
            
            if (processed_rows % 5000 == 0) {
                show_progress(bytes_read, file_size, "  - parsing trades  ");
            }

            if (line.empty() || line.find_first_not_of("\r\n\t ") == std::string::npos) continue;

            trade_signal sig;
            if (!parse_trade_line_fast(line.c_str(), sig) || 
                (sig.direction != 1 && sig.direction != -1) || 
                !is_valid_double(sig.target_weight) || 
                !is_valid_double(sig.alpha_bps)) {
                skipped_rows++;
                continue;
            }

            avg_alpha += sig.alpha_bps;
            signals.push_back(sig);
        }
        show_progress(file_size, file_size, "  - parsing trades  ");
        
        if (skipped_rows > 0) {
            LOG_WARN("skipped " + std::to_string(skipped_rows) + " malformed rows in trade logs");
        }

        std::sort(signals.begin(), signals.end(), [](const trade_signal& a, const trade_signal& b) {
            return a.timestamp < b.timestamp;
        });

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end_time - start_time;

        if (!signals.empty()) {
            LOG_INFO("trades load complete in " + std::to_string(elapsed.count()) + "s");
            std::cout << "  - total trades:   " << signals.size() << "\n";
            std::cout << "  - avg raw alpha:  " << std::fixed << std::setprecision(2) << (avg_alpha / signals.size()) << " bps\n";
        }
        return signals;
    }
};

class q_learning_oracle {
private:
    run_config cfg_;
    orderbook_discretizer discretizer_;
    std::vector<std::vector<double>> q_table_;

public:
    explicit q_learning_oracle(const run_config& cfg) 
        : cfg_(cfg), discretizer_(cfg) {
        q_table_.resize(orderbook_discretizer::num_states, 
                        std::vector<double>(orderbook_discretizer::num_actions, 0.0));
    }

    void train(const std::vector<l2_tick>& ticks) {
        if (ticks.size() < static_cast<size_t>(cfg_.lookahead_ticks + 1)) return;
        LOG_STEP("training q-learning oracle");
        
        std::cout << "  - config:         init_lr=" << cfg_.lr << " init_eps=" << cfg_.epsilon << "\n";
        std::cout << "  - episodes:       " << cfg_.episodes << "\n";

        const std::vector<double> action_participation = {0.01, 0.05, 0.10};

        std::mt19937 rng(42);
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        std::uniform_int_distribution<int> act_dist(0, orderbook_discretizer::num_actions - 1);

        double current_lr = cfg_.lr;
        double current_eps = cfg_.epsilon;

        for (int ep = 0; ep < cfg_.episodes; ++ep) {
            for (size_t i = 0; i < ticks.size() - cfg_.lookahead_ticks; ++i) {
                int state = discretizer_.get_state(ticks[i]);
                int action = (dist(rng) < current_eps) ? act_dist(rng) :
                    std::distance(q_table_[state].begin(), std::max_element(q_table_[state].begin(), q_table_[state].end()));

                int next_state = discretizer_.get_state(ticks[i+1]);
                
                double participation = action_participation[action];
                double mid_price = (ticks[i].ask1_p + ticks[i].bid1_p) * 0.5;
                double future_mid_price = (ticks[i + cfg_.lookahead_ticks].ask1_p + ticks[i + cfg_.lookahead_ticks].bid1_p) * 0.5;
                
                double spread_bps = (ticks[i].ask1_p - ticks[i].bid1_p) / mid_price * 10000.0;
                double base_impact = 50.0; 
                double instant_cost = (spread_bps * 0.5) + (participation * base_impact);
                
                double price_drift_bps = std::abs(future_mid_price - mid_price) / mid_price * 10000.0;
                double delay_penalty = (1.0 - participation) * price_drift_bps; 

                double reward = -(instant_cost + delay_penalty);
                reward = std::clamp(reward, -cfg_.reward_clip, cfg_.reward_clip);

                double best_next_q = *std::max_element(q_table_[next_state].begin(), q_table_[next_state].end());
                q_table_[state][action] += current_lr * (reward + cfg_.gamma * best_next_q - q_table_[state][action]);
            }
            
            current_lr = std::max(cfg_.lr_min, current_lr * cfg_.lr_decay);
            current_eps = std::max(cfg_.epsilon_min, current_eps * cfg_.epsilon_decay);

            show_progress(ep + 1, cfg_.episodes, "  - training        ");
        }
        LOG_INFO("q-learning converged.");
    }

    [[nodiscard]] double get_dynamic_impact_multiplier(const l2_tick& tick) const {
        int state = discretizer_.get_state(tick);
        double max_q = *std::max_element(q_table_[state].begin(), q_table_[state].end());
        double learned_slip_bps = std::abs(max_q);
        
        return std::max(0.5, learned_slip_bps / cfg_.baseline_slip_bps); 
    }

    void print_markdown_q_table() const {
        std::cout << "\n  learned expected slippage (bps) | Q-Table:\n";
        std::cout << "  | market_state       | action_1% | action_5% | action_10%|\n";
        std::cout << "  | :----------------- | :-------- | :-------- | :-------- |\n";
        for (int s = 0; s < orderbook_discretizer::num_states; ++s) {
            std::cout << "  | " << std::left << std::setw(18) << orderbook_discretizer::state_to_string(s) << " | ";
            for (int a = 0; a < orderbook_discretizer::num_actions; ++a) {
                std::cout << std::fixed << std::setprecision(2) << std::setw(9) << std::abs(q_table_[s][a]) << " | ";
            }
            std::cout << "\n";
        }
    }
};

class capacity_estimator {
public:
    [[nodiscard]] static double run_simulation(
        const std::vector<trade_signal>& signals,
        const std::vector<l2_tick>& ticks,
        const q_learning_oracle& oracle,
        const run_config& cfg)
    {
        LOG_STEP("executing capacity scaling search");
        if (ticks.empty() || signals.empty()) return 0.0;

        std::cout << "  - precomputing tick indices...\n";
        std::vector<size_t> trade_tick_indices;
        trade_tick_indices.reserve(signals.size());
        auto compare_ts = [](const l2_tick& t, uint64_t ts) { return t.timestamp < ts; };
        
        for (const auto& sig : signals) {
            auto it = std::lower_bound(ticks.begin(), ticks.end(), sig.timestamp, compare_ts);
            if (it != ticks.begin() && (it == ticks.end() || it->timestamp > sig.timestamp)) {
                it = std::prev(it);
            }
            trade_tick_indices.push_back(std::distance(ticks.begin(), it));
        }

        double best_aum = 0;
        double max_net_pnl = -1e12;
        
        size_t total_steps = static_cast<size_t>((cfg.aum_max - cfg.aum_min) / cfg.aum_step) + 1;
        size_t step_count = 0;

        for (double current_aum = cfg.aum_min; current_aum <= cfg.aum_max; current_aum += cfg.aum_step) {
            double total_gross_pnl = 0.0;
            double total_impact_cost = 0.0;

            for (size_t i = 0; i < signals.size(); ++i) {
                const auto& sig = signals[i];
                const auto& tick = ticks[trade_tick_indices[i]];
                
                double trade_size = current_aum * sig.target_weight;
                double gross_pnl = trade_size * (sig.alpha_bps / 10000.0);
                
                double market_state_multiplier = oracle.get_dynamic_impact_multiplier(tick);
                
                double scaled_impact_fraction = cfg.permanent_impact_coeff * market_state_multiplier * 
                                                std::pow(trade_size / cfg.daily_adv, cfg.impact_exponent);
                
                double participation_rate = trade_size / cfg.daily_adv;
                double liquidity_penalty = 1.0;
                if (participation_rate > cfg.max_participation_rate) {
                    liquidity_penalty += std::pow((participation_rate - cfg.max_participation_rate) * 15.0, 2.0); 
                }

                double final_impact_fraction = scaled_impact_fraction * liquidity_penalty;
                double trade_impact_cost = trade_size * final_impact_fraction;
                
                total_gross_pnl += gross_pnl;
                total_impact_cost += trade_impact_cost;
            }

            double net_pnl = total_gross_pnl - total_impact_cost;
            if (net_pnl > max_net_pnl) {
                max_net_pnl = net_pnl;
                best_aum = current_aum;
            } 
            
            step_count++;
            show_progress(step_count, total_steps, "  - simulating      ");
        }
        
        LOG_INFO("capacity search concluded");
        std::cout << "  - peak net pnl:   " << std::fixed << std::setprecision(0) << max_net_pnl << " rmb\n";
        std::cout << "  - peak capacity:  " << std::fixed << std::setprecision(2) << (best_aum / 1e8) << " billion rmb\n";
        return best_aum;
    }
};

run_config parse_cli(int argc, char* argv[]) {
    run_config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--l2" && i + 1 < argc) cfg.l2_path = argv[++i];
        else if (arg == "--trades" && i + 1 < argc) cfg.trades_path = argv[++i];
        else if (arg == "--vol" && i + 1 < argc) cfg.daily_vol = std::stod(argv[++i]);
        else if (arg == "--adv" && i + 1 < argc) cfg.daily_adv = std::stod(argv[++i]);
        else if (arg == "--spread-th" && i + 1 < argc) cfg.spread_th = std::stod(argv[++i]);
        else if (arg == "--imb-th" && i + 1 < argc) cfg.imb_th = std::stod(argv[++i]);
        else if (arg == "--lr" && i + 1 < argc) cfg.lr = std::stod(argv[++i]);
        else if (arg == "--lr-decay" && i + 1 < argc) cfg.lr_decay = std::stod(argv[++i]);
        else if (arg == "--eps" && i + 1 < argc) cfg.epsilon = std::stod(argv[++i]);
        else if (arg == "--eps-decay" && i + 1 < argc) cfg.epsilon_decay = std::stod(argv[++i]);
        else if (arg == "--episodes" && i + 1 < argc) cfg.episodes = std::stoi(argv[++i]);
        else if (arg == "--lookahead" && i + 1 < argc) cfg.lookahead_ticks = std::stoi(argv[++i]);
        else if (arg == "--aum-max" && i + 1 < argc) cfg.aum_max = std::stod(argv[++i]);
        else if (arg == "--aum-step" && i + 1 < argc) cfg.aum_step = std::stod(argv[++i]);
        else if (arg == "--base-impact" && i + 1 < argc) cfg.permanent_impact_coeff = std::stod(argv[++i]);
    }
    return cfg;
}

} 

int main(int argc, char* argv[]) {
    using namespace quant;

    if (argc < 3) {
        std::cerr << "usage: ./capacity_estimator --l2 <file.csv> --trades <file.csv> [options]\n";
        std::cerr << "options:\n";
        std::cerr << "  --vol <float>        daily volatility (default: 0.015)\n";
        std::cerr << "  --adv <float>        daily volume (default: 1e10)\n";
        std::cerr << "  --lookahead <int>    future ticks for slippage (default: 10)\n";
        std::cerr << "  --base-impact <float> default market impact coeff (default: 0.15)\n";
        return 1;
    }

    try {
        auto cfg = parse_cli(argc, argv);
        if (cfg.l2_path.empty() || cfg.trades_path.empty()) {
            LOG_ERROR("missing mandatory arguments: --l2 and --trades");
            return 1;
        }

        auto ticks = data_loader::load_l2_csv(cfg.l2_path);
        auto trades = data_loader::load_trades_csv(cfg.trades_path);

        q_learning_oracle oracle(cfg);
        oracle.train(ticks);
        oracle.print_markdown_q_table();

        capacity_estimator::run_simulation(trades, ticks, oracle, cfg);

        LOG_INFO("process completed gracefully.");
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("fatal error: ") + e.what());
        return 1;
    }

    return 0;
}
