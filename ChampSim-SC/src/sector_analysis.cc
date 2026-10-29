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

SectorTracker::SectorTracker()
{
    reset();
}

void SectorTracker::reset()
{
    history_table.clear();
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
}
