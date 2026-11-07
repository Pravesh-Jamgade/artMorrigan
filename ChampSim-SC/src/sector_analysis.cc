#include "cache.h"
#include "sector_analysis.h"
#include <algorithm>

uint32_t get_rrc_bucket_idx(uint64_t count)
{
    if (count == 0) return 0;
    uint32_t bucket = 0;
    uint64_t bound = 1;
    while (bound < count && bucket < 5) {
        bound <<= 1;
        bucket++;
    }
    // bucket is 0..5 (corresponding to 1, 2, 4, 8, 16, 32+)
    // Returns 1..6 so that index 0 is reserved for rrc0 (no re-reference)
    return bucket + 1;
}

// SAFAR Two-Level Cache-Cost Predictor Implementation
template<typename T>
static inline T clamp_val(T val, T min_val, T max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

SafarCostPredictor::SafarCostPredictor()
{
    full_reset();
}

void SafarCostPredictor::full_reset()
{
    for (int i = 0; i < SAFAR_STUDENT_SIZE; ++i) {
        student[i] = SAFAR_STUDENT_INIT; // 1 (weak cheap)
        student_last_sector[i] = 0xFFFFFFFFFFFFFFFFULL;
    }
    for (int r = 0; r < SAFAR_RRC_BUCKETS; ++r) {
        for (int fp = 0; fp < STLB_PTES_PER_BLOCK; ++fp) {
            teacher[r][fp] = SAFAR_TEACHER_INIT; // 128 (neutral)
        }
    }
    reset_evaluation_stats();
}

void SafarCostPredictor::reset_evaluation_stats()
{
    TP = FP = FN = TN = 0;
    l2_pred_cheap = l2_pred_costly = 0;
    llc_pred_cheap = llc_pred_costly = 0;
    dram_pred_cheap = dram_pred_costly = 0;

    student_increments = student_decrements = student_neutrals = 0;
    student_saturations_at_0 = student_saturations_at_3 = 0;

    teacher_updates = teacher_correct_no_update = 0;
    teacher_l2_decrements = teacher_llc_increments = teacher_dram_increments = 0;

    total_installations = total_eligible_evictions = total_evaluated = 0;
    skipped_invalid_source = skipped_empty = student_collisions = 0;
    residual_stats = ResidualStats();
}

bool SafarCostPredictor::predict(uint64_t sector_addr) const
{
    uint32_t index = hash_sector(sector_addr) % SAFAR_STUDENT_SIZE;
    uint8_t counter = student[index];
    return (counter >= 2);
}

void SafarCostPredictor::record_installation(bool valid, uint64_t sector_addr)
{
    if (valid) {
        total_installations++;
        uint32_t index = hash_sector(sector_addr) % SAFAR_STUDENT_SIZE;
        if (student_last_sector[index] != 0xFFFFFFFFFFFFFFFFULL && student_last_sector[index] != sector_addr) {
            student_collisions++;
        }
        student_last_sector[index] = sector_addr;
    } else {
        skipped_invalid_source++;
    }
}

void SafarCostPredictor::record_skipped_empty()
{
    skipped_empty++;
}

void SafarCostPredictor::train_and_evaluate(uint64_t sector_addr, bool predicted_costly, uint8_t leaf_source, uint32_t rrc_idx, uint32_t footprint)
{
    total_eligible_evictions++;
    if (leaf_source < 2 || leaf_source > 4) {
        skipped_invalid_source++;
        return;
    }
    total_evaluated++;

    // Ground truth: Cheap (0) is L2 (leaf_source == 2); Costly (1) is LLC or DRAM (leaf_source == 3 or 4)
    bool actual_costly = (leaf_source != 2);

    // 1. Confusion Matrix (Costly is Positive, Cheap is Negative)
    if (predicted_costly && actual_costly)
        TP++;
    else if (predicted_costly && !actual_costly)
        FP++;
    else if (!predicted_costly && actual_costly)
        FN++;
    else
        TN++;

    // Breakdown by source
    if (leaf_source == 2) {
        if (predicted_costly) l2_pred_costly++;
        else l2_pred_cheap++;
    } else if (leaf_source == 3) {
        if (predicted_costly) llc_pred_costly++;
        else llc_pred_cheap++;
    } else if (leaf_source == 4) {
        if (predicted_costly) dram_pred_costly++;
        else dram_pred_cheap++;
    }

    // 2. Teacher training: on all events (including matching predictions)
    uint32_t r = (rrc_idx < SAFAR_RRC_BUCKETS) ? rrc_idx : (SAFAR_RRC_BUCKETS - 1);
    uint32_t fp = (footprint >= 1 && footprint <= STLB_PTES_PER_BLOCK) ? (footprint - 1) : 0;

    int delta = 0;
    if (leaf_source == 2) {
        delta = -1;
        teacher_l2_decrements++;
    } else if (leaf_source == 3) {
        delta = +1;
        teacher_llc_increments++;
    } else if (leaf_source == 4) {
        delta = +2;
        teacher_dram_increments++;
    }
    int cur_t = teacher[r][fp];
    teacher[r][fp] = static_cast<uint8_t>(clamp_val<int>(cur_t + delta, 0, 255));
    teacher_updates++;

    // 3. Student update: guided by updated teacher value outside [128 - x, 128 + x]
    uint8_t teacher_value = teacher[r][fp];
    int u = 0;
    if (teacher_value > SAFAR_TEACHER_INIT + SAFAR_TEACHER_MARGIN_X)
        u = +1;
    else if (teacher_value < SAFAR_TEACHER_INIT - SAFAR_TEACHER_MARGIN_X)
        u = -1;
    else
        u = 0;

    uint32_t s_idx = hash_sector(sector_addr) % SAFAR_STUDENT_SIZE;
    uint8_t cur_s = student[s_idx];

    if (u == +1) {
        student_increments++;
        if (cur_s == 3) student_saturations_at_3++;
    } else if (u == -1) {
        student_decrements++;
        if (cur_s == 0) student_saturations_at_0++;
    } else {
        student_neutrals++;
    }

    student[s_idx] = static_cast<uint8_t>(clamp_val<int>(static_cast<int>(cur_s) + u, 0, 3));
}

void SafarCostPredictor::print_stats(std::ostream &out, uint32_t cpu) const
{
    double accuracy = (total_evaluated > 0) ? (double)(TP + TN) / (double)total_evaluated : 0.0;
    double precision = (TP + FP > 0) ? (double)TP / (double)(TP + FP) : 0.0;
    double recall = (TP + FN > 0) ? (double)TP / (double)(TP + FN) : 0.0;
    double f1 = (precision + recall > 0.0) ? (2.0 * precision * recall) / (precision + recall) : 0.0;
    double fpr = (FP + TN > 0) ? (double)FP / (double)(FP + TN) : 0.0;
    double fnr = (FN + TP > 0) ? (double)FN / (double)(FN + TP) : 0.0;

    uint32_t s_states[4] = {0, 0, 0, 0};
    for (int i = 0; i < SAFAR_STUDENT_SIZE; ++i) {
        if (student[i] <= 3) s_states[student[i]]++;
    }

    uint32_t t_below = 0, t_equal = 0, t_above = 0;
    for (int r = 0; r < SAFAR_RRC_BUCKETS; ++r) {
        for (int fp = 0; fp < STLB_PTES_PER_BLOCK; ++fp) {
            if (teacher[r][fp] < SAFAR_TEACHER_INIT) t_below++;
            else if (teacher[r][fp] == SAFAR_TEACHER_INIT) t_equal++;
            else t_above++;
        }
    }

    const char *rrc_labels[7] = {"0", "1", "2", "4", "8", "16", "32"};

    out << "\n=== SAFAR TWO-LEVEL CACHE-COST PREDICTOR (CPU " << cpu << ") ===\n";
    out << "Note: Evictions during the measurement interval may evaluate sectors installed during warmup.\n";
    out << "--- Confusion Matrix (Positive = Costly [LLC/DRAM], Negative = Cheap [L2]) ---\n";
    out << "Total Evaluated Predictions: " << total_evaluated << "\n";
    out << "True Positives  (TP): " << TP << "\n";
    out << "False Positives (FP): " << FP << "\n";
    out << "False Negatives (FN): " << FN << "\n";
    out << "True Negatives  (TN): " << TN << "\n";
    out << std::fixed << std::setprecision(4);
    out << "Accuracy:  " << accuracy << "\n";
    out << "Precision: " << precision << "\n";
    out << "Recall:    " << recall << "\n";
    out << "F1 Score:  " << f1 << "\n";
    out << "FPR:       " << fpr << "\n";
    out << "FNR:       " << fnr << "\n";

    out << "\n--- Leaf-PTE Source Breakdown ---\n";
    out << "L2   (Cheap)  - Total: " << (l2_pred_cheap + l2_pred_costly)
        << ", Predicted Cheap (TN): " << l2_pred_cheap
        << ", Predicted Costly (FP): " << l2_pred_costly << "\n";
    out << "LLC  (Costly) - Total: " << (llc_pred_cheap + llc_pred_costly)
        << ", Predicted Cheap (FN): " << llc_pred_cheap
        << ", Predicted Costly (TP): " << llc_pred_costly << "\n";
    out << "DRAM (Costly) - Total: " << (dram_pred_cheap + dram_pred_costly)
        << ", Predicted Cheap (FN): " << dram_pred_cheap
        << ", Predicted Costly (TP): " << dram_pred_costly << "\n";

    out << "\n--- Student Table Statistics (Size: " << SAFAR_STUDENT_SIZE << ") ---\n";
    out << "Entries in State 0 (Strong Cheap): " << s_states[0] << "\n";
    out << "Entries in State 1 (Weak Cheap):   " << s_states[1] << "\n";
    out << "Entries in State 2 (Weak Costly):  " << s_states[2] << "\n";
    out << "Entries in State 3 (Strong Costly):" << s_states[3] << "\n";
    out << "Total Increments:         " << student_increments << "\n";
    out << "Total Decrements:         " << student_decrements << "\n";
    out << "Neutral Updates:          " << student_neutrals << "\n";
    out << "Saturations at 0:         " << student_saturations_at_0 << "\n";
    out << "Saturations at 3:         " << student_saturations_at_3 << "\n";
    out << "Index Collisions:         " << student_collisions << "\n";

    out << "\n--- Teacher Table Statistics (Dimensions: 7 x " << STLB_PTES_PER_BLOCK << ") ---\n";
    out << "Confidence Deadband [128 - x, 128 + x]: [" << (SAFAR_TEACHER_INIT - SAFAR_TEACHER_MARGIN_X)
        << ", " << (SAFAR_TEACHER_INIT + SAFAR_TEACHER_MARGIN_X) << "] (x = " << SAFAR_TEACHER_MARGIN_X << ")\n";
    out << "Total Teacher Updates:    " << teacher_updates << "\n";
    out << "Correct Predictions (No Update): " << teacher_correct_no_update << "\n";
    out << "L2-Driven Decrements:     " << teacher_l2_decrements << "\n";
    out << "LLC-Driven Increments:    " << teacher_llc_increments << "\n";
    out << "DRAM-Driven Increments:   " << teacher_dram_increments << "\n";
    out << "Entries < 128 (Cheap):    " << t_below << "\n";
    out << "Entries = 128 (Neutral):  " << t_equal << "\n";
    out << "Entries > 128 (Costly):   " << t_above << "\n";

    out << "Teacher Matrix (RRC x Footprint):\n";
    out << std::setw(12) << "RRC \\ FP";
    for (int fp = 1; fp <= STLB_PTES_PER_BLOCK; ++fp) {
        out << std::setw(6) << ("fp" + std::to_string(fp));
    }
    out << "\n";
    for (int r = 0; r < SAFAR_RRC_BUCKETS; ++r) {
        out << std::setw(12) << ("rrc" + std::string(rrc_labels[r]));
        for (int fp = 0; fp < STLB_PTES_PER_BLOCK; ++fp) {
            out << std::setw(6) << (uint32_t)teacher[r][fp];
        }
        out << "\n";
    }

    out << "\n--- Lifecycle & Population Statistics ---\n";
    out << "Total Installations (Valid): " << total_installations << "\n";
    out << "Total Eligible Evictions:    " << total_eligible_evictions << "\n";
    out << "Total Evaluated Predictions: " << total_evaluated << "\n";
    out << "Skipped (Invalid Source):    " << skipped_invalid_source << "\n";
    out << "Skipped (Empty Footprint):   " << skipped_empty << "\n";
    out << "Residual Resident Population: " << residual_stats.total << "\n";
    out << "  Residual Predicted Cheap:  " << residual_stats.pred_cheap << "\n";
    out << "  Residual Predicted Costly: " << residual_stats.pred_costly << "\n";
    out << "  Residual Leaf L2:          " << residual_stats.leaf_l2 << "\n";
    out << "  Residual Leaf LLC:         " << residual_stats.leaf_llc << "\n";
    out << "  Residual Leaf DRAM:        " << residual_stats.leaf_dram << "\n";
}

void SafarCostPredictor::print_csv_stats(std::ostream &csv, uint32_t cpu) const
{
    double accuracy = (total_evaluated > 0) ? (double)(TP + TN) / (double)total_evaluated : 0.0;
    double precision = (TP + FP > 0) ? (double)TP / (double)(TP + FP) : 0.0;
    double recall = (TP + FN > 0) ? (double)TP / (double)(TP + FN) : 0.0;
    double f1 = (precision + recall > 0.0) ? (2.0 * precision * recall) / (precision + recall) : 0.0;
    double fpr = (FP + TN > 0) ? (double)FP / (double)(FP + TN) : 0.0;
    double fnr = (FN + TP > 0) ? (double)FN / (double)(FN + TP) : 0.0;

    uint32_t s_states[4] = {0, 0, 0, 0};
    for (int i = 0; i < SAFAR_STUDENT_SIZE; ++i) {
        if (student[i] <= 3) s_states[student[i]]++;
    }

    uint32_t t_below = 0, t_equal = 0, t_above = 0;
    for (int r = 0; r < SAFAR_RRC_BUCKETS; ++r) {
        for (int fp = 0; fp < STLB_PTES_PER_BLOCK; ++fp) {
            if (teacher[r][fp] < SAFAR_TEACHER_INIT) t_below++;
            else if (teacher[r][fp] == SAFAR_TEACHER_INIT) t_equal++;
            else t_above++;
        }
    }

    const char *rrc_labels[7] = {"0", "1", "2", "4", "8", "16", "32"};

    csv << "SAFAR_PREDICTOR_EVALUATED,cpu," << cpu << "," << total_evaluated << "\n";
    csv << "SAFAR_PREDICTOR_TP,cpu," << cpu << "," << TP << "\n";
    csv << "SAFAR_PREDICTOR_FP,cpu," << cpu << "," << FP << "\n";
    csv << "SAFAR_PREDICTOR_FN,cpu," << cpu << "," << FN << "\n";
    csv << "SAFAR_PREDICTOR_TN,cpu," << cpu << "," << TN << "\n";

    csv << std::fixed << std::setprecision(4);
    csv << "SAFAR_PREDICTOR_ACCURACY,cpu," << cpu << "," << accuracy << "\n";
    csv << "SAFAR_PREDICTOR_PRECISION,cpu," << cpu << "," << precision << "\n";
    csv << "SAFAR_PREDICTOR_RECALL,cpu," << cpu << "," << recall << "\n";
    csv << "SAFAR_PREDICTOR_F1,cpu," << cpu << "," << f1 << "\n";
    csv << "SAFAR_PREDICTOR_FPR,cpu," << cpu << "," << fpr << "\n";
    csv << "SAFAR_PREDICTOR_FNR,cpu," << cpu << "," << fnr << "\n";

    csv << "SAFAR_PREDICTOR_L2_TOTAL,cpu," << cpu << "," << (l2_pred_cheap + l2_pred_costly) << "\n";
    csv << "SAFAR_PREDICTOR_L2_PRED_CHEAP,cpu," << cpu << "," << l2_pred_cheap << "\n";
    csv << "SAFAR_PREDICTOR_L2_PRED_COSTLY,cpu," << cpu << "," << l2_pred_costly << "\n";
    csv << "SAFAR_PREDICTOR_LLC_TOTAL,cpu," << cpu << "," << (llc_pred_cheap + llc_pred_costly) << "\n";
    csv << "SAFAR_PREDICTOR_LLC_PRED_CHEAP,cpu," << cpu << "," << llc_pred_cheap << "\n";
    csv << "SAFAR_PREDICTOR_LLC_PRED_COSTLY,cpu," << cpu << "," << llc_pred_costly << "\n";
    csv << "SAFAR_PREDICTOR_DRAM_TOTAL,cpu," << cpu << "," << (dram_pred_cheap + dram_pred_costly) << "\n";
    csv << "SAFAR_PREDICTOR_DRAM_PRED_CHEAP,cpu," << cpu << "," << dram_pred_cheap << "\n";
    csv << "SAFAR_PREDICTOR_DRAM_PRED_COSTLY,cpu," << cpu << "," << dram_pred_costly << "\n";

    csv << "SAFAR_PREDICTOR_STUDENT_STATE_0,cpu," << cpu << "," << s_states[0] << "\n";
    csv << "SAFAR_PREDICTOR_STUDENT_STATE_1,cpu," << cpu << "," << s_states[1] << "\n";
    csv << "SAFAR_PREDICTOR_STUDENT_STATE_2,cpu," << cpu << "," << s_states[2] << "\n";
    csv << "SAFAR_PREDICTOR_STUDENT_STATE_3,cpu," << cpu << "," << s_states[3] << "\n";
    csv << "SAFAR_PREDICTOR_STUDENT_INCREMENTS,cpu," << cpu << "," << student_increments << "\n";
    csv << "SAFAR_PREDICTOR_STUDENT_DECREMENTS,cpu," << cpu << "," << student_decrements << "\n";
    csv << "SAFAR_PREDICTOR_STUDENT_NEUTRALS,cpu," << cpu << "," << student_neutrals << "\n";
    csv << "SAFAR_PREDICTOR_STUDENT_SAT_0,cpu," << cpu << "," << student_saturations_at_0 << "\n";
    csv << "SAFAR_PREDICTOR_STUDENT_SAT_3,cpu," << cpu << "," << student_saturations_at_3 << "\n";
    csv << "SAFAR_PREDICTOR_STUDENT_COLLISIONS,cpu," << cpu << "," << student_collisions << "\n";

    csv << "SAFAR_PREDICTOR_TEACHER_MARGIN_X,cpu," << cpu << "," << SAFAR_TEACHER_MARGIN_X << "\n";
    csv << "SAFAR_PREDICTOR_TEACHER_UPDATES,cpu," << cpu << "," << teacher_updates << "\n";
    csv << "SAFAR_PREDICTOR_TEACHER_CORRECT_NO_UPDATE,cpu," << cpu << "," << teacher_correct_no_update << "\n";
    csv << "SAFAR_PREDICTOR_TEACHER_L2_DECREMENTS,cpu," << cpu << "," << teacher_l2_decrements << "\n";
    csv << "SAFAR_PREDICTOR_TEACHER_LLC_INCREMENTS,cpu," << cpu << "," << teacher_llc_increments << "\n";
    csv << "SAFAR_PREDICTOR_TEACHER_DRAM_INCREMENTS,cpu," << cpu << "," << teacher_dram_increments << "\n";
    csv << "SAFAR_PREDICTOR_TEACHER_BELOW_128,cpu," << cpu << "," << t_below << "\n";
    csv << "SAFAR_PREDICTOR_TEACHER_EQUAL_128,cpu," << cpu << "," << t_equal << "\n";
    csv << "SAFAR_PREDICTOR_TEACHER_ABOVE_128,cpu," << cpu << "," << t_above << "\n";

    csv << "SAFAR_PREDICTOR_INSTALLATIONS,cpu," << cpu << "," << total_installations << "\n";
    csv << "SAFAR_PREDICTOR_ELIGIBLE_EVICTIONS,cpu," << cpu << "," << total_eligible_evictions << "\n";
    csv << "SAFAR_PREDICTOR_SKIPPED_INVALID_SOURCE,cpu," << cpu << "," << skipped_invalid_source << "\n";
    csv << "SAFAR_PREDICTOR_SKIPPED_EMPTY,cpu," << cpu << "," << skipped_empty << "\n";
    csv << "SAFAR_PREDICTOR_RESIDUAL_RESIDENT,cpu," << cpu << "," << residual_stats.total << "\n";
    csv << "SAFAR_PREDICTOR_RESIDUAL_PRED_CHEAP,cpu," << cpu << "," << residual_stats.pred_cheap << "\n";
    csv << "SAFAR_PREDICTOR_RESIDUAL_PRED_COSTLY,cpu," << cpu << "," << residual_stats.pred_costly << "\n";
    csv << "SAFAR_PREDICTOR_RESIDUAL_LEAF_L2,cpu," << cpu << "," << residual_stats.leaf_l2 << "\n";
    csv << "SAFAR_PREDICTOR_RESIDUAL_LEAF_LLC,cpu," << cpu << "," << residual_stats.leaf_llc << "\n";
    csv << "SAFAR_PREDICTOR_RESIDUAL_LEAF_DRAM,cpu," << cpu << "," << residual_stats.leaf_dram << "\n";

    csv << "SAFAR_PREDICTOR_TEACHER_MATRIX,cpu," << cpu << "\n";
    csv << "rrc";
    for (int fp = 1; fp <= STLB_PTES_PER_BLOCK; ++fp) csv << ",fp" << fp;
    csv << "\n";
    for (int r = 0; r < SAFAR_RRC_BUCKETS; ++r) {
        csv << "rrc" << rrc_labels[r];
        for (int fp = 0; fp < STLB_PTES_PER_BLOCK; ++fp) {
            csv << "," << (uint32_t)teacher[r][fp];
        }
        csv << "\n";
    }
}

SectorTracker::SectorTracker()
{
    cost_predictor.full_reset();
    reset();
}

void SectorTracker::init_sets(uint32_t num_sets)
{
    set_trackers.resize(num_sets);
    for (auto &st : set_trackers) {
        st.reset();
    }
}

void SectorTracker::record_sector_access(uint32_t set, uint64_t sector_tag, uint8_t hit_where)
{
    if (set >= set_trackers.size()) return;
    SetSectorRDTracker &st = set_trackers[set];

    // Source index: 0 = All, 1 = L2 (hit_where == 2), 2 = LLC (hit_where == 3), 3 = DRAM (hit_where == 4)
    int src_idx = -1;
    if (hit_where == 2) src_idx = 1;
    else if (hit_where == 3) src_idx = 2;
    else if (hit_where == 4) src_idx = 3;

    // 1. Normal Per-Set Reuse Distance
    // Search for sector_tag in unbounded lru_stack
    auto it = std::find(st.lru_stack.begin(), st.lru_stack.end(), sector_tag);
    if (it != st.lru_stack.end()) {
        uint64_t normal_dist = std::distance(st.lru_stack.begin(), it);
        int bin = 0;
        if (normal_dist <= 5) {
            bin = static_cast<int>(normal_dist); // 0, 1, 2, 3, 4, 5
        } else if (normal_dist <= 7) {
            bin = 6;
        } else if (normal_dist <= 15) {
            bin = 7;
        } else if (normal_dist <= 31) {
            bin = 8;
        } else if (normal_dist <= 63) {
            bin = 9;
        } else {
            bin = 10;
        }

        normal_rd_hist[0][bin]++;
        if (src_idx != -1) {
            normal_rd_hist[src_idx][bin]++;
        }

        st.lru_stack.erase(it);
    }
    // Sector is now accessed, move to top (MRU) of lru_stack
    st.lru_stack.insert(st.lru_stack.begin(), sector_tag);

    // 2. Post-Eviction Reuse Distance
    // Check if this sector was previously evicted and has an active evict_seq
    auto ev_it = st.evict_seq.find(sector_tag);
    if (ev_it != st.evict_seq.end()) {
        uint64_t te = ev_it->second;
        // Count distinct same-set sectors accessed since eviction time te
        // (these are sectors whose last_seen_seq > te, starting from the second element in stack
        // since the top element is sector_tag itself)
        uint64_t post_dist = 0;
        for (size_t i = 1; i < st.lru_stack.size(); ++i) {
            uint64_t other_tag = st.lru_stack[i];
            auto l_it = st.last_seen_seq.find(other_tag);
            if (l_it != st.last_seen_seq.end() && l_it->second > te) {
                post_dist++;
            }
        }

        int bin = 0;
        if (post_dist == 0) bin = 0;
        else if (post_dist == 1) bin = 1;
        else if (post_dist == 2) bin = 2;
        else if (post_dist <= 4) bin = 3;
        else if (post_dist <= 8) bin = 4;
        else if (post_dist <= 16) bin = 5;
        else if (post_dist <= 32) bin = 6;
        else if (post_dist <= 64) bin = 7;
        else bin = 8;

        post_evict_rd_hist[0][bin]++;
        if (src_idx != -1) {
            post_evict_rd_hist[src_idx][bin]++;
        }

        st.evict_seq.erase(ev_it);
    }

    // Update monotonic counter and last_seen_seq
    st.set_access_counter++;
    st.last_seen_seq[sector_tag] = st.set_access_counter;
}

void SectorTracker::record_sector_eviction(uint32_t set, uint64_t sector_tag)
{
    if (set >= set_trackers.size()) return;
    SetSectorRDTracker &st = set_trackers[set];
    st.evict_seq[sector_tag] = st.set_access_counter;
}

void SectorTracker::reset()
{
    cost_predictor.reset_evaluation_stats();
    history_table.clear();
    for (auto &st : set_trackers) {
        st.reset();
    }
    for (int s = 0; s < 4; ++s) {
        for (int b = 0; b < NUM_NORMAL_RD_BINS; ++b) {
            normal_rd_hist[s][b] = 0;
        }
        for (int b = 0; b < NUM_POST_EVICT_RD_BINS; ++b) {
            post_evict_rd_hist[s][b] = 0;
        }
    }
    total_sector_misses = 0;
    total_repeated_sector_miss_events = 0;
    repeated_sectors_llc_events = 0;
    repeated_sectors_dram_events = 0;

    for (int b = 0; b < 3; ++b) {
        for (int r = 0; r < 7; ++r) {
            for (int fp = 0; fp < STLB_PTES_PER_BLOCK; ++fp) {
                repeat2_rrc_footprint[b][r][fp] = 0;
                repeat3_rrc_footprint[b][r][fp] = 0;
                expensive_repeat2_rrc_footprint[b][r][fp] = 0;
                expensive_repeat3_rrc_footprint[b][r][fp] = 0;
            }
        }
    }

    for (int r1 = 0; r1 < 7; ++r1) {
        for (int r2 = 0; r2 < 7; ++r2) {
            rrc_transitions[r1][r2] = 0;
        }
    }

    for (int f1 = 0; f1 < STLB_PTES_PER_BLOCK; ++f1) {
        for (int f2 = 0; f2 < STLB_PTES_PER_BLOCK; ++f2) {
            footprint_transitions[f1][f2] = 0;
        }
    }

    for (int s1 = 0; s1 < 3; ++s1) {
        for (int s2 = 0; s2 < 3; ++s2) {
            fill_source_transitions[s1][s2] = 0;
        }
    }

    for (int r = 0; r < 7; ++r) {
        for (int fp = 0; fp < STLB_PTES_PER_BLOCK; ++fp) {
            for (int s = 0; s < 3; ++s) {
                prev_features_to_next_source[r][fp][s] = 0;
            }
        }
    }

    total_same_offset_repeat_misses = 0;
    total_diff_offset_repeat_misses = 0;
}

void SectorTracker::record_miss(uint64_t vpn, bool is_non_compulsory)
{
    uint64_t sector_id = get_sector_id(vpn);
    SectorHistory &sh = history_table[sector_id];

    sh.total_misses++;
    total_sector_misses++;

    if (sh.total_misses >= 2) {
        total_repeated_sector_miss_events++;
    }

    if (is_non_compulsory) {
        sh.noncompulsory_misses++;
    }

    // Offset analysis within sector
    uint32_t offset = vpn % STLB_PTES_PER_BLOCK;
    if (sh.missed_offset_bitmap & (1u << offset)) {
        sh.same_offset_repeat_misses++;
        total_same_offset_repeat_misses++;
    } else {
        if (sh.missed_offset_bitmap != 0) {
            sh.diff_offset_repeat_misses++;
            total_diff_offset_repeat_misses++;
        }
        sh.missed_offset_bitmap |= (1u << offset);
    }
}

void SectorTracker::record_fill_source(uint64_t vpn, uint8_t hit_where)
{
    if (hit_where < 2 || hit_where > 4) return; // Only L2, LLC, DRAM
    uint64_t sector_id = get_sector_id(vpn);
    SectorHistory &sh = history_table[sector_id];

    if (hit_where == 2) {
        sh.l2_bound_misses++;
    } else if (hit_where == 3) {
        sh.llc_bound_misses++;
        sh.expensive_misses++;
    } else if (hit_where == 4) {
        sh.dram_bound_misses++;
        sh.expensive_misses++;
    }

    if (sh.total_misses >= 2) {
        if (hit_where == 3) repeated_sectors_llc_events++;
        else if (hit_where == 4) repeated_sectors_dram_events++;
    }

    // If there is a previous completed residency snapshot, record predictive transition:
    // (previous_RRC_bucket, previous_footprint) -> next_fill_source
    if (sh.last_residency.valid) {
        uint32_t prev_rrc_idx = get_rrc_bucket_idx(sh.last_residency.rrc);
        uint32_t prev_fp_idx = (sh.last_residency.footprint >= 1 && sh.last_residency.footprint <= STLB_PTES_PER_BLOCK)
                                ? (sh.last_residency.footprint - 1) : 0;
        uint32_t next_src_idx = hit_where - 2;
        prev_features_to_next_source[prev_rrc_idx][prev_fp_idx][next_src_idx]++;
    }
}

void SectorTracker::record_completed_residency(uint64_t sector_tag, uint32_t rrc, uint32_t footprint, uint8_t fill_source)
{
    if (fill_source < 2 || fill_source > 4) return;
    if (footprint < 1 || footprint > STLB_PTES_PER_BLOCK) return;

    SectorHistory &sh = history_table[sector_tag];
    const uint32_t bound_idx = fill_source - 2; // 0: L2, 1: LLC, 2: DRAM
    const uint32_t rrc_idx = get_rrc_bucket_idx(rrc);
    const uint32_t fp_idx = footprint - 1;

    // Condition 1: repeat2 (sector has experienced >= 2 STLB misses)
    if (sh.total_misses >= 2) {
        repeat2_rrc_footprint[bound_idx][rrc_idx][fp_idx]++;
    }

    // Condition 2: repeat3 (sector has experienced >= 3 STLB misses)
    if (sh.total_misses >= 3) {
        repeat3_rrc_footprint[bound_idx][rrc_idx][fp_idx]++;
    }

    // Condition 3: expensive_repeat2 (at least 2 expensive fills)
    if (sh.expensive_misses >= 2) {
        expensive_repeat2_rrc_footprint[bound_idx][rrc_idx][fp_idx]++;
    }

    // Condition 4: expensive_repeat3 (at least 3 expensive fills)
    if (sh.expensive_misses >= 3) {
        expensive_repeat3_rrc_footprint[bound_idx][rrc_idx][fp_idx]++;
    }

    // Behavioral persistence transitions across residencies
    if (sh.last_residency.valid) {
        uint32_t prev_rrc_idx = get_rrc_bucket_idx(sh.last_residency.rrc);
        uint32_t prev_fp_idx = (sh.last_residency.footprint >= 1 && sh.last_residency.footprint <= STLB_PTES_PER_BLOCK)
                                ? (sh.last_residency.footprint - 1) : 0;
        uint32_t prev_src_idx = sh.last_residency.fill_source - 2;

        rrc_transitions[prev_rrc_idx][rrc_idx]++;
        footprint_transitions[prev_fp_idx][fp_idx]++;
        fill_source_transitions[prev_src_idx][bound_idx]++;
    }

    // Update snapshot for this sector
    sh.last_residency.rrc = rrc;
    sh.last_residency.footprint = footprint;
    sh.last_residency.fill_source = fill_source;
    sh.last_residency.valid = true;
}

void SectorTracker::print_stats(std::ostream &out, uint32_t cpu) const
{
    uint64_t unique_sectors = history_table.size();
    uint64_t sectors_miss_ge2 = 0;
    uint64_t sectors_miss_ge3 = 0;
    uint64_t sectors_expensive_ge2 = 0;
    uint64_t sectors_expensive_ge3 = 0;

    // Histogram bins: 1, 2, 3-4, 5-8, 9-16, 17-32, 33+
    uint64_t hist_unique[7] = {0};
    uint64_t hist_l2[7] = {0};
    uint64_t hist_llc[7] = {0};
    uint64_t hist_dram[7] = {0};

    auto get_bin = [](uint64_t m) -> int {
        if (m <= 1) return 0;
        if (m == 2) return 1;
        if (m <= 4) return 2;
        if (m <= 8) return 3;
        if (m <= 16) return 4;
        if (m <= 32) return 5;
        return 6;
    };

    for (const auto &kv : history_table) {
        const SectorHistory &sh = kv.second;
        if (sh.total_misses >= 2) sectors_miss_ge2++;
        if (sh.total_misses >= 3) sectors_miss_ge3++;
        if (sh.expensive_misses >= 2) sectors_expensive_ge2++;
        if (sh.expensive_misses >= 3) sectors_expensive_ge3++;

        int bin = get_bin(sh.total_misses);
        hist_unique[bin]++;
        hist_l2[bin] += sh.l2_bound_misses;
        hist_llc[bin] += sh.llc_bound_misses;
        hist_dram[bin] += sh.dram_bound_misses;
    }

    out << "\n=== SAFAR SECTOR-LEVEL MISS REPEATABILITY & PERSISTENCE (CPU " << cpu << ") ===\n";
    out << "sector_repeat_unique_sectors: " << unique_sectors << "\n";
    out << "sector_repeat_sectors_with_ge2_misses: " << sectors_miss_ge2 << "\n";
    out << "sector_repeat_sectors_with_ge3_misses: " << sectors_miss_ge3 << "\n";
    out << "sector_repeat_total_miss_events_repeated_sectors: " << total_repeated_sector_miss_events << "\n";
    out << "sector_repeat_llc_events_repeated_sectors: " << repeated_sectors_llc_events << "\n";
    out << "sector_repeat_dram_events_repeated_sectors: " << repeated_sectors_dram_events << "\n";
    out << "sector_repeat_sectors_with_ge2_expensive_refills: " << sectors_expensive_ge2 << "\n";
    out << "sector_repeat_sectors_with_ge3_expensive_refills: " << sectors_expensive_ge3 << "\n";
    out << "sector_repeat_same_offset_repeat_misses: " << total_same_offset_repeat_misses << "\n";
    out << "sector_repeat_diff_offset_repeat_misses: " << total_diff_offset_repeat_misses << "\n";

    out << "\nOffline Retrospective Analysis - Sector Miss-Frequency Histogram:\n";
    out << std::setw(15) << "Miss Episodes" << std::setw(15) << "Unique Sectors"
        << std::setw(15) << "L2 Events" << std::setw(15) << "LLC Events" << std::setw(15) << "DRAM Events" << "\n";
    const char *bin_names[7] = {"1", "2", "3-4", "5-8", "9-16", "17-32", "33+"};
    for (int i = 0; i < 7; ++i) {
        out << std::setw(15) << bin_names[i]
            << std::setw(15) << hist_unique[i]
            << std::setw(15) << hist_l2[i]
            << std::setw(15) << hist_llc[i]
            << std::setw(15) << hist_dram[i] << "\n";
    }

    const char* bound_names[3] = {"L2", "LLC", "DRAM"};
    const char* rrc_labels[7] = {"0", "1", "2", "4", "8", "16", "32"};

    out << "\nRepeat-Conditioned RRC x Footprint Matrices (repeat3):\n";
    for (int b = 0; b < 3; ++b) {
        out << "  " << bound_names[b] << "_sector_repeat3_rrc_footprint:\n";
        for (int r = 0; r < 7; ++r) {
            out << "    rrc" << rrc_labels[r] << ":";
            for (int fp = 0; fp < STLB_PTES_PER_BLOCK; ++fp) {
                out << " " << repeat3_rrc_footprint[b][r][fp];
            }
            out << "\n";
        }
    }

    out << "\nExpensive Repeat-Conditioned RRC x Footprint Matrices (expensive_repeat2):\n";
    for (int b = 1; b < 3; ++b) { // LLC and DRAM
        out << "  " << bound_names[b] << "_expensive_repeat2_rrc_footprint:\n";
        for (int r = 0; r < 7; ++r) {
            out << "    rrc" << rrc_labels[r] << ":";
            for (int fp = 0; fp < STLB_PTES_PER_BLOCK; ++fp) {
                out << " " << expensive_repeat2_rrc_footprint[b][r][fp];
            }
            out << "\n";
        }
    }

    out << "\nBehavioral Persistence - RRC Transitions (rows=prev, cols=curr):\n";
    for (int r1 = 0; r1 < 7; ++r1) {
        out << "  prev_rrc" << rrc_labels[r1] << ":";
        for (int r2 = 0; r2 < 7; ++r2) {
            out << " " << rrc_transitions[r1][r2];
        }
        out << "\n";
    }

    out << "\nBehavioral Persistence - Footprint Transitions (rows=prev, cols=curr):\n";
    for (int f1 = 0; f1 < STLB_PTES_PER_BLOCK; ++f1) {
        out << "  prev_fp" << (f1 + 1) << ":";
        for (int f2 = 0; f2 < STLB_PTES_PER_BLOCK; ++f2) {
            out << " " << footprint_transitions[f1][f2];
        }
        out << "\n";
    }

    out << "\nBehavioral Persistence - Fill Source Transitions (rows=prev, cols=curr):\n";
    for (int s1 = 0; s1 < 3; ++s1) {
        out << "  prev_" << bound_names[s1] << ":";
        for (int s2 = 0; s2 < 3; ++s2) {
            out << " " << fill_source_transitions[s1][s2];
        }
        out << "\n";
    }

    // Normal Per-Set Reuse Distance Histogram Table
    const char *norm_bin_labels[NUM_NORMAL_RD_BINS] = {
        "0", "1", "2", "3", "4", "5", "6-7", "8-15", "16-31", "32-63", "64+"
    };
    out << "\nNormal Per-Set Reuse Distance (Unbounded LRU Stack):\n";
    out << std::setw(12) << "Distance"
        << std::setw(15) << "All_Sectors"
        << std::setw(15) << "L2_Cheap"
        << std::setw(15) << "LLC_Costly"
        << std::setw(15) << "DRAM_Costly" << "\n";
    for (int i = 0; i < NUM_NORMAL_RD_BINS; ++i) {
        out << std::setw(12) << norm_bin_labels[i]
            << std::setw(15) << normal_rd_hist[0][i]
            << std::setw(15) << normal_rd_hist[1][i]
            << std::setw(15) << normal_rd_hist[2][i]
            << std::setw(15) << normal_rd_hist[3][i] << "\n";
    }

    // Post-Eviction Reuse Distance Histogram Table
    const char *post_bin_labels[NUM_POST_EVICT_RD_BINS] = {
        "0", "1", "2", "3-4", "5-8", "9-16", "17-32", "33-64", "65+"
    };
    out << "\nPost-Eviction Per-Set Reuse Distance:\n";
    out << std::setw(12) << "Distance"
        << std::setw(15) << "All_Sectors"
        << std::setw(15) << "L2_Cheap"
        << std::setw(15) << "LLC_Costly"
        << std::setw(15) << "DRAM_Costly" << "\n";
    for (int i = 0; i < NUM_POST_EVICT_RD_BINS; ++i) {
        out << std::setw(12) << post_bin_labels[i]
            << std::setw(15) << post_evict_rd_hist[0][i]
            << std::setw(15) << post_evict_rd_hist[1][i]
            << std::setw(15) << post_evict_rd_hist[2][i]
            << std::setw(15) << post_evict_rd_hist[3][i] << "\n";
    }

    cost_predictor.print_stats(out, cpu);
}

void SectorTracker::print_csv_stats(std::ostream &csv, uint32_t cpu) const
{
    const string prefix = "Core_" + std::to_string(cpu) + "_";
    uint64_t unique_sectors = history_table.size();
    uint64_t sectors_miss_ge2 = 0;
    uint64_t sectors_miss_ge3 = 0;
    uint64_t sectors_expensive_ge2 = 0;
    uint64_t sectors_expensive_ge3 = 0;

    uint64_t hist_unique[7] = {0};
    uint64_t hist_l2[7] = {0};
    uint64_t hist_llc[7] = {0};
    uint64_t hist_dram[7] = {0};

    auto get_bin = [](uint64_t m) -> int {
        if (m <= 1) return 0;
        if (m == 2) return 1;
        if (m <= 4) return 2;
        if (m <= 8) return 3;
        if (m <= 16) return 4;
        if (m <= 32) return 5;
        return 6;
    };

    for (const auto &kv : history_table) {
        const SectorHistory &sh = kv.second;
        if (sh.total_misses >= 2) sectors_miss_ge2++;
        if (sh.total_misses >= 3) sectors_miss_ge3++;
        if (sh.expensive_misses >= 2) sectors_expensive_ge2++;
        if (sh.expensive_misses >= 3) sectors_expensive_ge3++;

        int bin = get_bin(sh.total_misses);
        hist_unique[bin]++;
        hist_l2[bin] += sh.l2_bound_misses;
        hist_llc[bin] += sh.llc_bound_misses;
        hist_dram[bin] += sh.dram_bound_misses;
    }

    csv << prefix << "sector_repeat_unique_sectors," << unique_sectors << "\n";
    csv << prefix << "sector_repeat_sectors_with_ge2_misses," << sectors_miss_ge2 << "\n";
    csv << prefix << "sector_repeat_sectors_with_ge3_misses," << sectors_miss_ge3 << "\n";
    csv << prefix << "sector_repeat_total_miss_events_repeated_sectors," << total_repeated_sector_miss_events << "\n";
    csv << prefix << "sector_repeat_llc_events_repeated_sectors," << repeated_sectors_llc_events << "\n";
    csv << prefix << "sector_repeat_dram_events_repeated_sectors," << repeated_sectors_dram_events << "\n";
    csv << prefix << "sector_repeat_sectors_with_ge2_expensive_refills," << sectors_expensive_ge2 << "\n";
    csv << prefix << "sector_repeat_sectors_with_ge3_expensive_refills," << sectors_expensive_ge3 << "\n";
    csv << prefix << "sector_repeat_same_offset_repeat_misses," << total_same_offset_repeat_misses << "\n";
    csv << prefix << "sector_repeat_diff_offset_repeat_misses," << total_diff_offset_repeat_misses << "\n";

    // Sector miss-frequency histogram table
    csv << "SECTOR_MISS_FREQUENCY_HISTOGRAM,cpu," << cpu << "\n";
    csv << "episodes,unique_sectors,l2_events,llc_events,dram_events\n";
    const char *bin_names[7] = {"1", "2", "3-4", "5-8", "9-16", "17-32", "33+"};
    for (int i = 0; i < 7; ++i) {
        csv << bin_names[i] << "," << hist_unique[i] << "," << hist_l2[i] << "," << hist_llc[i] << "," << hist_dram[i] << "\n";
    }

    const char* bound_names[3] = {"L2", "LLC", "DRAM"};
    const char* rrc_labels[7] = {"0", "1", "2", "4", "8", "16", "32"};

    // Repeat-conditioned matrices (repeat2, repeat3, expensive_repeat2, expensive_repeat3)
    auto print_mat = [&](const string &name, const uint64_t mat[3][7][STLB_PTES_PER_BLOCK], int b_start, int b_end) {
        for (int b = b_start; b <= b_end; ++b) {
            csv << name << "_" << bound_names[b] << ",cpu," << cpu << "\n";
            csv << "rrc";
            for (int fp = 1; fp <= STLB_PTES_PER_BLOCK; ++fp) csv << "," << fp;
            csv << "\n";
            for (int r = 0; r < 7; ++r) {
                csv << "rrc" << rrc_labels[r];
                for (int fp = 0; fp < STLB_PTES_PER_BLOCK; ++fp) {
                    csv << "," << mat[b][r][fp];
                }
                csv << "\n";
            }
        }
    };

    print_mat("SECTOR_REPEAT2_RRC_FOOTPRINT", repeat2_rrc_footprint, 0, 2);
    print_mat("SECTOR_REPEAT3_RRC_FOOTPRINT", repeat3_rrc_footprint, 0, 2);
    print_mat("SECTOR_EXPENSIVE_REPEAT2_RRC_FOOTPRINT", expensive_repeat2_rrc_footprint, 1, 2);
    print_mat("SECTOR_EXPENSIVE_REPEAT3_RRC_FOOTPRINT", expensive_repeat3_rrc_footprint, 1, 2);

    // Persistence matrices
    csv << "SECTOR_RRC_TRANSITIONS,cpu," << cpu << "\n";
    csv << "prev_rrc";
    for (int r = 0; r < 7; ++r) csv << ",curr_rrc" << rrc_labels[r];
    csv << "\n";
    for (int r1 = 0; r1 < 7; ++r1) {
        csv << "rrc" << rrc_labels[r1];
        for (int r2 = 0; r2 < 7; ++r2) csv << "," << rrc_transitions[r1][r2];
        csv << "\n";
    }

    csv << "SECTOR_FOOTPRINT_TRANSITIONS,cpu," << cpu << "\n";
    csv << "prev_fp";
    for (int fp = 1; fp <= STLB_PTES_PER_BLOCK; ++fp) csv << ",curr_fp" << fp;
    csv << "\n";
    for (int f1 = 0; f1 < STLB_PTES_PER_BLOCK; ++f1) {
        csv << (f1 + 1);
        for (int f2 = 0; f2 < STLB_PTES_PER_BLOCK; ++f2) csv << "," << footprint_transitions[f1][f2];
        csv << "\n";
    }

    csv << "SECTOR_FILL_SOURCE_TRANSITIONS,cpu," << cpu << "\n";
    csv << "prev_source,curr_l2,curr_llc,curr_dram\n";
    for (int s1 = 0; s1 < 3; ++s1) {
        csv << bound_names[s1];
        for (int s2 = 0; s2 < 3; ++s2) csv << "," << fill_source_transitions[s1][s2];
        csv << "\n";
    }

    // Predictive conditional table: (previous_RRC_bucket, previous_footprint) -> next_fill_source
    csv << "SECTOR_PREV_FEATURES_TO_NEXT_SOURCE,cpu," << cpu << "\n";
    csv << "prev_rrc,prev_fp,next_l2,next_llc,next_dram\n";
    for (int r = 0; r < 7; ++r) {
        for (int fp = 0; fp < STLB_PTES_PER_BLOCK; ++fp) {
            csv << "rrc" << rrc_labels[r] << "," << (fp + 1)
                << "," << prev_features_to_next_source[r][fp][0]
                << "," << prev_features_to_next_source[r][fp][1]
                << "," << prev_features_to_next_source[r][fp][2] << "\n";
        }
    }

    // Normal Per-Set Reuse Distance CSV Table
    const char *norm_bin_labels[NUM_NORMAL_RD_BINS] = {
        "0", "1", "2", "3", "4", "5", "6-7", "8-15", "16-31", "32-63", "64+"
    };
    csv << "SECTOR_NORMAL_SET_REUSE_DISTANCE,cpu," << cpu << "\n";
    csv << "distance,all_sectors,l2_cheap,llc_costly,dram_costly\n";
    for (int i = 0; i < NUM_NORMAL_RD_BINS; ++i) {
        csv << norm_bin_labels[i] << "," << normal_rd_hist[0][i] << "," << normal_rd_hist[1][i]
            << "," << normal_rd_hist[2][i] << "," << normal_rd_hist[3][i] << "\n";
    }

    // Post-Eviction Reuse Distance CSV Table
    const char *post_bin_labels[NUM_POST_EVICT_RD_BINS] = {
        "0", "1", "2", "3-4", "5-8", "9-16", "17-32", "33-64", "65+"
    };
    csv << "SECTOR_POST_EVICTION_SET_REUSE_DISTANCE,cpu," << cpu << "\n";
    csv << "distance,all_sectors,l2_cheap,llc_costly,dram_costly\n";
    for (int i = 0; i < NUM_POST_EVICT_RD_BINS; ++i) {
        csv << post_bin_labels[i] << "," << post_evict_rd_hist[0][i] << "," << post_evict_rd_hist[1][i]
            << "," << post_evict_rd_hist[2][i] << "," << post_evict_rd_hist[3][i] << "\n";
    }

    cost_predictor.print_csv_stats(csv, cpu);
}
