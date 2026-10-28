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
#define STLB_PTES_PER_BLOCK 4
#endif

// Forward declaration of rrc_bucket helper (shared with cache.cc)
uint32_t get_rrc_bucket_idx(uint64_t count);

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

// Comprehensive manager for SAFAR sector-level miss repeatability and persistence analysis
class SectorTracker {
public:
    // Sector history table mapping canonical sector identifier to SectorHistory
    std::unordered_map<uint64_t, SectorHistory> history_table;

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

    // Print reports
    void print_stats(std::ostream &out, uint32_t cpu) const;
    void print_csv_stats(std::ostream &csv, uint32_t cpu) const;
};

#endif // SECTOR_ANALYSIS_H
