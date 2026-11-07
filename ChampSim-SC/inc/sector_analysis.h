#ifndef SECTOR_ANALYSIS_H
#define SECTOR_ANALYSIS_H

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include "champsim.h"

#ifndef STLB_PTES_PER_BLOCK
#error "STLB_PTES_PER_BLOCK must be defined before including sector_analysis.h"
#endif

// Forward declaration of rrc_bucket helper (shared with cache.cc)
uint32_t get_rrc_bucket_idx(uint64_t count);

// Residual resident population statistics at end of simulation
struct ResidualStats {
    uint64_t total = 0;
    uint64_t pred_cheap = 0;
    uint64_t pred_costly = 0;
    uint64_t leaf_l2 = 0;
    uint64_t leaf_llc = 0;
    uint64_t leaf_dram = 0;
};

// SAFAR Two-Level Cache-Cost Predictor constants
constexpr int SAFAR_STUDENT_SIZE = 1024;
constexpr int SAFAR_RRC_BUCKETS = 7;
constexpr uint8_t SAFAR_TEACHER_INIT = 128;
constexpr uint8_t SAFAR_STUDENT_INIT = 1;
constexpr int SAFAR_TEACHER_MARGIN_X = 25; // Deadband [128 - x, 128 + x] = [103, 153]

// Two-level predictor classifying leaf-PTE sources into Cheap (L2) vs Costly (LLC/DRAM)
class SafarCostPredictor {
public:
    // Student table: 1024 2-bit saturating counters (0=Strong Cheap, 1=Weak Cheap, 2=Weak Costly, 3=Strong Costly)
    uint8_t student[SAFAR_STUDENT_SIZE];

    // Teacher table: 7 RRC buckets x STLB_PTES_PER_BLOCK footprint matrix of 8-bit unsigned saturating counters
    uint8_t teacher[SAFAR_RRC_BUCKETS][STLB_PTES_PER_BLOCK];

    // Student table index collision tracker
    uint64_t student_last_sector[SAFAR_STUDENT_SIZE];

    // Evaluated prediction counters (Confusion Matrix: Costly is Positive, Cheap is Negative)
    uint64_t TP = 0;
    uint64_t FP = 0;
    uint64_t FN = 0;
    uint64_t TN = 0;

    // Breakdown by actual leaf-PTE source
    uint64_t l2_pred_cheap = 0;   // TN
    uint64_t l2_pred_costly = 0;  // FP
    uint64_t llc_pred_cheap = 0;  // part of FN
    uint64_t llc_pred_costly = 0; // part of TP
    uint64_t dram_pred_cheap = 0; // part of FN
    uint64_t dram_pred_costly = 0;// part of TP

    // Student statistics
    uint64_t student_increments = 0;
    uint64_t student_decrements = 0;
    uint64_t student_neutrals = 0;
    uint64_t student_saturations_at_0 = 0;
    uint64_t student_saturations_at_3 = 0;

    // Teacher statistics
    uint64_t teacher_updates = 0;
    uint64_t teacher_correct_no_update = 0;
    uint64_t teacher_l2_decrements = 0;
    uint64_t teacher_llc_increments = 0;
    uint64_t teacher_dram_increments = 0;

    // Training / lifecycle statistics
    uint64_t total_installations = 0;
    uint64_t total_eligible_evictions = 0;
    uint64_t total_evaluated = 0;
    uint64_t skipped_invalid_source = 0;
    uint64_t skipped_empty = 0;
    uint64_t student_collisions = 0;

    // Residual resident population at end of simulation
    ResidualStats residual_stats;

    SafarCostPredictor();
    void full_reset();
    void reset_evaluation_stats();

    // Normalized sector address hashing
    static inline uint32_t hash_sector(uint64_t addr) {
        addr ^= (addr >> 21) ^ (addr >> 42);
        addr *= 0x9e3779b97f4a7c15ULL;
        return static_cast<uint32_t>(addr);
    }

    // Prediction at STLB sector installation
    bool predict(uint64_t sector_addr) const;

    // Lifecycle events
    void record_installation(bool valid, uint64_t sector_addr);
    void record_skipped_empty();

    // Evaluation and training at STLB sector eviction
    void train_and_evaluate(uint64_t sector_addr, bool predicted_costly, uint8_t leaf_source, uint32_t rrc_idx, uint32_t footprint);

    // Reporting
    void print_stats(std::ostream &out, uint32_t cpu) const;
    void print_csv_stats(std::ostream &csv, uint32_t cpu) const;
};

// Structure representing the snapshot of a completed STLB residency
struct ResidencySnapshot {
    uint32_t rrc = 0;
    uint8_t footprint = 0;
    uint8_t fill_source = 0; // 2: L2, 3: LLC, 4: DRAM
    bool valid = false;
};

// Characterization-only history for a translation sector
struct SectorHistory {
    uint64_t total_misses = 0;
    uint64_t noncompulsory_misses = 0;

    uint64_t l2_bound_misses = 0;
    uint64_t llc_bound_misses = 0;
    uint64_t dram_bound_misses = 0;

    uint64_t expensive_misses = 0; // LLC + DRAM

    // Constituent VPN/PTE offsets within the sector that have caused misses
    uint8_t missed_offset_bitmap = 0;
    uint64_t same_offset_repeat_misses = 0;
    uint64_t diff_offset_repeat_misses = 0;

    // Snapshot of the previous completed STLB residency for transition tracking
    ResidencySnapshot last_residency;
};

// Constants for Per-Set Reuse Distance histograms
constexpr int NUM_NORMAL_RD_BINS = 11;
constexpr int NUM_POST_EVICT_RD_BINS = 9;

// Per-set tracking for Normal RD (unbounded stack) and Post-Eviction RD
struct SetSectorRDTracker {
    std::vector<uint64_t> lru_stack;                      // Unbounded LRU stack of sector tags (index 0 is MRU)
    std::unordered_map<uint64_t, uint64_t> last_seen_seq; // Access sequence number when sector was referenced in this set
    std::unordered_map<uint64_t, uint64_t> evict_seq;     // Sequence number when sector was evicted from STLB
    uint64_t set_access_counter = 0;                      // Monotonic counter of sector references to this set

    void reset() {
        lru_stack.clear();
        last_seen_seq.clear();
        evict_seq.clear();
        set_access_counter = 0;
    }
};

// Comprehensive manager for SAFAR sector-level miss repeatability and persistence analysis
class SectorTracker {
public:
    // Sector history table mapping canonical sector identifier to SectorHistory
    std::unordered_map<uint64_t, SectorHistory> history_table;

    // Per-set trackers for reuse distance
    std::vector<SetSectorRDTracker> set_trackers;

    // Reuse distance histograms broken down by leaf-PTE refill source:
    // [0: All, 1: L2, 2: LLC, 3: DRAM][bin]
    uint64_t normal_rd_hist[4][NUM_NORMAL_RD_BINS];
    uint64_t post_evict_rd_hist[4][NUM_POST_EVICT_RD_BINS];

    // Aggregate statistics
    uint64_t total_sector_misses = 0;
    uint64_t total_repeated_sector_miss_events = 0; // events where total_misses >= 2 at the time of miss
    uint64_t repeated_sectors_llc_events = 0;       // LLC-bound fills belonging to sectors with >= 2 misses
    uint64_t repeated_sectors_dram_events = 0;      // DRAM-bound fills belonging to sectors with >= 2 misses

    // Repeat-conditioned RRC x Footprint matrices:
    // Indexed by: [bound: 0=L2, 1=LLC, 2=DRAM][rrc_idx: 0=rrc0, 1..6=rrc1..32+][footprint - 1: 0..STLB_PTES_PER_BLOCK-1]
    uint64_t repeat2_rrc_footprint[3][7][STLB_PTES_PER_BLOCK];
    uint64_t repeat3_rrc_footprint[3][7][STLB_PTES_PER_BLOCK];
    uint64_t expensive_repeat2_rrc_footprint[3][7][STLB_PTES_PER_BLOCK];
    uint64_t expensive_repeat3_rrc_footprint[3][7][STLB_PTES_PER_BLOCK];

    // Behavioral persistence matrices:
    // A. RRC transitions: [previous_RRC_bucket: 0..6][current_RRC_bucket: 0..6]
    uint64_t rrc_transitions[7][7];

    // B. Footprint transitions: [previous_footprint - 1: 0..STLB_PTES_PER_BLOCK-1][current_footprint - 1: 0..STLB_PTES_PER_BLOCK-1]
    uint64_t footprint_transitions[STLB_PTES_PER_BLOCK][STLB_PTES_PER_BLOCK];

    // C. Fill-source transitions: [previous_source: 0=L2, 1=LLC, 2=DRAM][current_source: 0=L2, 1=LLC, 2=DRAM]
    uint64_t fill_source_transitions[3][3];

    // Predictive conditional table:
    // (previous_RRC_bucket, previous_footprint) -> next_fill_source
    // Dimensions: [previous_rrc: 0..6][previous_footprint - 1: 0..STLB_PTES_PER_BLOCK-1][next_source: 0=L2, 1=LLC, 2=DRAM]
    uint64_t prev_features_to_next_source[7][STLB_PTES_PER_BLOCK][3];

    // Same-offset vs different-offset repetition counters
    uint64_t total_same_offset_repeat_misses = 0;
    uint64_t total_diff_offset_repeat_misses = 0;

    // Two-level cost predictor (SAFAR)
    SafarCostPredictor cost_predictor;

    SectorTracker();
    void reset();

    // Canonical sector identifier calculation
    static inline uint64_t get_sector_id(uint64_t vpn) {
        return vpn / STLB_PTES_PER_BLOCK;
    }

    // Called on every STLB miss (at va_to_pa)
    void record_miss(uint64_t vpn, bool is_non_compulsory);

    // Called when the leaf-PTE service source is determined in va_to_pa
    void record_fill_source(uint64_t vpn, uint8_t hit_where);

    // Called at STLB eviction (completed residency)
    void record_completed_residency(uint64_t sector_tag, uint32_t rrc, uint32_t footprint, uint8_t fill_source);

    // Reuse distance tracking per set
    void init_sets(uint32_t num_sets);
    void record_sector_access(uint32_t set, uint64_t sector_tag, uint8_t hit_where);
    void record_sector_eviction(uint32_t set, uint64_t sector_tag);

    // Print reports
    void print_stats(std::ostream &out, uint32_t cpu) const;
    void print_csv_stats(std::ostream &csv, uint32_t cpu) const;
};

#endif // SECTOR_ANALYSIS_H
