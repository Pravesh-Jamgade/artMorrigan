#ifndef CONFIG_PARAMS_H
#define CONFIG_PARAMS_H

#include <iostream>

inline void print_config_file_params() {
    std::cout << "=== CONFIG FILE KEY-VALUES ===" << std::endl;
    std::cout << "config_file: default.ini" << std::endl;
    std::cout << "[build] branch_predictor = hashed_perceptron" << std::endl;
    std::cout << "[build] l1i_prefetcher = next_line" << std::endl;
    std::cout << "[build] l1d_prefetcher = next_line" << std::endl;
    std::cout << "[build] l2c_prefetcher = spp_dev" << std::endl;
    std::cout << "[build] llc_prefetcher = no" << std::endl;
    std::cout << "[build] llc_replacement = lru" << std::endl;
    std::cout << "[build] cores = 1" << std::endl;
    std::cout << "[build] stlb_prefetcher = no" << std::endl;
    std::cout << "[build] name_suffix = " << std::endl;
    std::cout << "[simulator] page_size = 4kb" << std::endl;
    std::cout << "[simulator] stlb_sets = 256" << std::endl;
    std::cout << "[simulator] stlb_assoc = 6" << std::endl;
    std::cout << "[simulator] stlb_latency = 8" << std::endl;
    std::cout << "[simulator] ptw_start_level = l2" << std::endl;
    std::cout << "[simulator] stlb_mode = analysis" << std::endl;
    std::cout << "[simulator] stlb_ptes_per_block = 4" << std::endl;
    std::cout << "[simulator] pq_size = 64" << std::endl;
    std::cout << "[simulator] prefetch_to_tlb = 0" << std::endl;
    std::cout << "[simulator] free_prefetching = 0" << std::endl;
    std::cout << "[simulator] free_prefetching_prefetch = 0" << std::endl;
    std::cout << "[simulator] asap = 0" << std::endl;
    std::cout << "[simulator] ideal = 0" << std::endl;
    std::cout << "[simulator] pc_table = 128" << std::endl;
    std::cout << "[simulator] pc_table_assoc = 128" << std::endl;
    std::cout << "[simulator] markov_sets = 256" << std::endl;
    std::cout << "[simulator] markov_assoc = 256" << std::endl;
    std::cout << "[simulator] lookahead_depth = 0" << std::endl;
    std::cout << "[simulator] successors = 2" << std::endl;
    std::cout << "[simulator] reset_frequency = 5000" << std::endl;
    std::cout << "[simulator] replacement_policy = 0" << std::endl;
    std::cout << "[simulator] successor_replacement_policy = 0" << std::endl;
    std::cout << "[simulator] lookahead_limit = 5" << std::endl;
    std::cout << "[simulator] confidence_bits = 3" << std::endl;
    std::cout << "[simulator] bp_filter = 0" << std::endl;
    std::cout << "[morrigan] s1_sets = 128" << std::endl;
    std::cout << "[morrigan] s1_assoc = 32" << std::endl;
    std::cout << "[morrigan] s2_sets = 128" << std::endl;
    std::cout << "[morrigan] s2_assoc = 32" << std::endl;
    std::cout << "[morrigan] s4_sets = 128" << std::endl;
    std::cout << "[morrigan] s4_assoc = 32" << std::endl;
    std::cout << "[morrigan] s8_sets = 64" << std::endl;
    std::cout << "[morrigan] s8_assoc = 16" << std::endl;
    std::cout << "[tage] h2_sets = 1024" << std::endl;
    std::cout << "[tage] h2_assoc = 4" << std::endl;
    std::cout << "[tage] h4_sets = 1024" << std::endl;
    std::cout << "[tage] h4_assoc = 4" << std::endl;
    std::cout << "[tage] h8_sets = 1024" << std::endl;
    std::cout << "[tage] h8_assoc = 4" << std::endl;
    std::cout << "[tage] allocation_policy = 1" << std::endl;
    std::cout << "[tage] usefulness_bit = 1" << std::endl;
    std::cout << "[tage] usefulness_reset = 65536" << std::endl;
    std::cout << "[tage] delta_history = 0" << std::endl;
    std::cout << "[tage] confidence_threshold = 1" << std::endl;
    std::cout << "==============================" << std::endl << std::endl;
}

#endif
