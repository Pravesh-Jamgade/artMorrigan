#ifndef LAMP_H
#define LAMP_H

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <iostream>
#include <iomanip>
#include "champsim.h"
#include "block.h"

// Forward declarations
class CACHE;

// Global configuration knobs controlling simulation modes
// knob_lamp: 1 = enable LAMP parallel leaf lookup and cancellation; 0 = conventional PTW only
extern uint8_t knob_lamp;
// knob_stlb: 1 = STLB enabled (LAMP races on STLB miss); 0 = STLB disabled (LAMP races on L1 ITLB/DTLB miss)
extern uint8_t knob_stlb;

// -----------------------------------------------------------------------------
// Translation Outcome Enumerations
// -----------------------------------------------------------------------------

// TransWinner: Identifies which parallel path supplied the valid translation first
enum TransWinner {
    WINNER_NONE = 0, // No winner yet (translation is still in-flight)
    WINNER_LAMP = 1, // LAMP found leaf PTE in L2/LLC first and satisfied translation
    WINNER_PTW  = 2  // PTW completed normal multi-level table walk first
};

// TransState: Enforces the single-completion state machine
// Transitions: PENDING -> COMPLETED_BY_LAMP or COMPLETED_BY_PTW (or CANCELED)
// Invariant: Exactly one path may transition out of PENDING to prevent double TLB fills.
enum TransState {
    STATE_PENDING = 0,           // In-flight parallel lookup/walk
    STATE_COMPLETED_BY_LAMP = 1, // LAMP completed translation; PTW marked for cancellation
    STATE_COMPLETED_BY_PTW = 2,  // PTW completed translation; LAMP marked for cancellation
    STATE_CANCELED = 3           // Transaction aborted prior to completion
};

// PTWStage: Identifies the architectural stage of the page table walk at cancellation
enum PTWStage {
    PTW_STAGE_NONE = 0, // Walk not started or uninitialized
    PTW_STAGE_PML4 = 1, // Level 4 Page Map Level 4 lookup
    PTW_STAGE_PDP  = 2, // Level 3 Page Directory Pointer lookup
    PTW_STAGE_PD   = 3, // Level 2 Page Directory lookup
    PTW_STAGE_LEAF = 4, // Level 1 Page Table (Leaf PTE) lookup
    PTW_STAGE_DONE = 5  // Walk finished naturally
};

// -----------------------------------------------------------------------------
// TranslationTransaction: Represents one shared translation request
// -----------------------------------------------------------------------------
// On a TLB miss, exactly one TranslationTransaction is instantiated with a unique trans_id.
// Both LAMP_PATH and PTW_PATH carry this transaction ID to synchronize completion
// and coordinate resource reclamation.
struct TranslationTransaction {
    uint64_t trans_id = 0;       // Unique 64-bit identifier for this translation event
    uint32_t cpu = 0;            // Core ID originating the translation miss
    uint64_t vpn = 0;            // Virtual Page Number (VA >> 12)
    uint64_t va = 0;             // Full 64-bit virtual address
    uint64_t ip = 0;             // Instruction Pointer of originating instruction
    uint64_t instr_id = 0;       // Unique instruction ID in the pipeline
    uint8_t  type = 0;           // Request type (LOAD, RFO, PREFETCH, etc.)
    bool     is_instruction = false; // true = ITLB miss (fetch); false = DTLB miss (load/store)
    uint8_t  requesting_tlb = 0; // Cache type ID of requesting TLB (IS_ITLB, IS_DTLB, IS_STLB)

    // State machine
    TransState  state = STATE_PENDING; // Current transaction state (atomic single-completion)
    TransWinner winner = WINNER_NONE;   // Path that completed first (WINNER_LAMP or WINNER_PTW)

    // Cycle timestamps for detailed timing and lead measurements
    uint64_t trigger_cycle = 0;             // Cycle when the translation miss occurred
    uint64_t lamp_ready_cycle = 0;          // Cycle when LAMP retrieved the leaf PTE (or UINT64_MAX if miss)
    uint64_t ptw_ready_cycle = 0;           // Cycle when PTW completed the translation walk
    uint64_t ptw_leaf_ready_cycle = 0;      // Cycle when PTW reached the leaf-PTE stage
    uint64_t completion_cycle = 0;          // Cycle when winner filled TLB and woke waiters
    uint64_t load_completion_cycle = 0;     // Cycle when dependent load instruction retired/executed
    uint64_t early_data_issue_cycle = 0;    // Cycle when physical memory access was issued early by LAMP
    uint64_t conventional_issue_cycle = 0;  // Cycle when physical access would have issued under PTW
    uint64_t validation_cycle = 0;          // Cycle when upper-level permission validation completed
    uint64_t prefetch_completion_cycle = 0; // Cycle when speculative prefetch completed

    // Translation result payload
    uint64_t translated_pa = 0;  // Full 64-bit translated physical address
    uint64_t translated_ppn = 0; // Translated Physical Page Number (PA >> 12)
    uint8_t  lamp_hit_where = 0; // Cache level where LAMP hit: 1 = L1D, 2 = L2C, 3 = LLC, 0xFF = Unusable, 0 = Miss
    bool     lamp_usable = false; // true if translation permissions and mappings are valid

    // PTW cancellation tracking
    PTWStage ptw_cancel_stage = PTW_STAGE_NONE; // PTW stage when LAMP canceled the walk
    int      ptw_requests_avoided = 0;          // Count of downstream PTW memory fetches suppressed

    // Ownership & double-fill prevention flags
    bool ptw_canceled = false;   // Set to true when PTW walk is aborted
    bool lamp_canceled = false;  // Set to true when LAMP lookup is aborted
    bool mshr_released = false;  // Ensures TLB miss MSHR is released exactly once
    bool tlb_filled = false;     // Ensures TLB cache array is filled exactly once (invariant == 0 duplicate)
};

// -----------------------------------------------------------------------------
// TimingHistogram: Fixed-bucket latency and lead-time distribution tracker
// -----------------------------------------------------------------------------
// Bins:
//   Positive buckets: [0, 1-4, 5-9, 10-19, 20-39, 40-79, 80-159, 160-319, 320+] cycles
//   Negative buckets: [-(1-4), -(5-9), -(10-19), -(20-39), -(40-79), -(80-159), -(160-319), -320+] cycles
class TimingHistogram {
public:
    uint64_t count = 0;               // Total number of recorded latency/lead samples
    uint64_t sum = 0;                 // Cumulative sum of absolute values for average calculation
    int64_t  min_val = 999999999;     // Minimum observed latency/lead value
    int64_t  max_val = -999999999;    // Maximum observed latency/lead value

    // Positive / non-negative buckets:
    // index 0: 0 cycles, 1: 1-4, 2: 5-9, 3: 10-19, 4: 20-39, 5: 40-79, 6: 80-159, 7: 160-319, 8: 320+ cycles
    uint64_t pos_buckets[9] = {0};

    // Negative buckets:
    // index 0: -(1-4), 1: -(5-9), 2: -(10-19), 3: -(20-39), 4: -(40-79), 5: -(80-159), 6: -(160-319), 7: -320+ cycles
    uint64_t neg_buckets[8] = {0};

    // record: Inserts a sample value into the min/max/sum trackers and corresponding histogram bin
    void record(int64_t val) {
        count++;                           // Increment total number of samples recorded in this histogram
        sum += (val >= 0 ? val : -val);    // Accumulate absolute magnitude of sample into running sum
        if (val < min_val) min_val = val;  // Update minimum observed sample value if current val is smaller
        if (val > max_val) max_val = val;  // Update maximum observed sample value if current val is larger

        if (val >= 0) {                    // Non-negative latency or positive lead-time sample
            if (val == 0) pos_buckets[0]++;      // Bucket 0: exact 0-cycle latency/lead
            else if (val <= 4) pos_buckets[1]++; // Bucket 1: 1 to 4 cycles
            else if (val <= 9) pos_buckets[2]++; // Bucket 2: 5 to 9 cycles
            else if (val <= 19) pos_buckets[3]++;// Bucket 3: 10 to 19 cycles
            else if (val <= 39) pos_buckets[4]++;// Bucket 4: 20 to 39 cycles
            else if (val <= 79) pos_buckets[5]++;// Bucket 5: 40 to 79 cycles
            else if (val <= 159) pos_buckets[6]++;// Bucket 6: 80 to 159 cycles
            else if (val <= 319) pos_buckets[7]++;// Bucket 7: 160 to 319 cycles
            else pos_buckets[8]++;               // Bucket 8: 320 or more cycles
        } else {                           // Negative lead-time sample (e.g., LAMP finished after PTW)
            int64_t mag = -val;                  // Convert negative value to positive magnitude for binning
            if (mag <= 4) neg_buckets[0]++;      // Neg Bucket 0: -1 to -4 cycles
            else if (mag <= 9) neg_buckets[1]++; // Neg Bucket 1: -5 to -9 cycles
            else if (mag <= 19) neg_buckets[2]++;// Neg Bucket 2: -10 to -19 cycles
            else if (mag <= 39) neg_buckets[3]++;// Neg Bucket 3: -20 to -39 cycles
            else if (mag <= 79) neg_buckets[4]++;// Neg Bucket 4: -40 to -79 cycles
            else if (mag <= 159) neg_buckets[5]++;// Neg Bucket 5: -80 to -159 cycles
            else if (mag <= 319) neg_buckets[6]++;// Neg Bucket 6: -160 to -319 cycles
            else neg_buckets[7]++;               // Neg Bucket 7: -320 or fewer cycles
        }
    }

    // avg: Computes the arithmetic mean of recorded samples
    double avg() const { return count ? (double)sum / count : 0.0; }

    // Formatted stdout display
    void print(std::ostream &out, const std::string &label) const;

    // Structured CSV output: scalars ("key,value") and 2-row vectors ("key,label1,...\nkey,val1,...")
    void print_csv(std::ostream &csv, const std::string &key_prefix) const;
};

// -----------------------------------------------------------------------------
// LampSideStats: Per-TLB metrics (tracked independently for ITLB and DTLB)
// -----------------------------------------------------------------------------
struct LampSideStats {
    // -------------------------------------------------------------------------
    // A. Lookup & Translation Winner
    // -------------------------------------------------------------------------
    uint64_t triggers = 0;          // Total translation misses triggering parallel LAMP/PTW
    uint64_t lamp_l1_hit = 0;       // Leaf PTE found directly in L1D cache (when PTW_START_LEVEL == 1)
    uint64_t lamp_l2_hit = 0;       // Leaf PTE found directly in L2 cache
    uint64_t lamp_llc_hit = 0;      // Leaf PTE found directly in LLC
    uint64_t lamp_miss = 0;         // Leaf PTE absent from searched caches (LAMP stops; no DRAM request)
    uint64_t lamp_hit_unusable = 0; // Leaf PTE found but lacked walk-derived upper-level permissions
    uint64_t lamp_wins = 0;         // LAMP completed translation before PTW
    uint64_t ptw_wins = 0;          // PTW completed translation before LAMP (or on LAMP miss)
    uint64_t lamp_tlb_fills = 0;    // TLB fills executed by LAMP winning path
    uint64_t ptw_tlb_fills = 0;     // TLB fills executed by PTW winning path
    uint64_t lamp_late_hit = 0;     // LAMP result arriving after PTW already completed
    uint64_t ptw_late_response = 0; // PTW callback arriving after LAMP already completed/canceled

    // -------------------------------------------------------------------------
    // B. Request Cancellation & Invariants
    // -------------------------------------------------------------------------
    uint64_t ptw_cancel_count = 0;        // Total PTW walks aborted by winning LAMP
    uint64_t lamp_cancel_count = 0;       // Total LAMP lookups aborted by winning PTW
    uint64_t ptw_cancel_pml4 = 0;         // PTW cancellations occurring at Level 4 (PML4) stage
    uint64_t ptw_cancel_pdp = 0;          // PTW cancellations occurring at Level 3 (PDP) stage
    uint64_t ptw_cancel_pd = 0;           // PTW cancellations occurring at Level 2 (PD) stage
    uint64_t ptw_cancel_leaf = 0;         // PTW cancellations occurring at Level 1 (Leaf PTE) stage
    uint64_t ptw_requests_avoided = 0;    // Downstream memory requests suppressed by canceling walks
    uint64_t ptw_inflight_wasted = 0;     // Already-issued memory requests that had to drain
    uint64_t rq_entries_released = 0;     // Read Queue entries safely removed upon cancellation
    uint64_t mshr_entries_released = 0;   // Exclusively-owned MSHR entries freed upon cancellation
    uint64_t mshr_subscribers_removed = 0;// Losing-path subscriber references removed from merged MSHRs
    uint64_t outstanding_uncancelable = 0;// In-flight memory requests that must drain without serving CPU
    uint64_t late_responses_ignored = 0;  // Late responses safely dropped without crashing or leaking
    uint64_t duplicate_tlb_fills = 0;     // Invariant: MUST ALWAYS BE 0 (proves no race double fills)
    uint64_t resource_leaks = 0;          // Invariant: MUST ALWAYS BE 0 (proves no lost MSHR/RQ resources)

    // -------------------------------------------------------------------------
    // C. Timing Distributions & Histograms
    // -------------------------------------------------------------------------
    TimingHistogram hist_lamp_lookup_latency;                // Trigger -> leaf PTE returned by LAMP
    TimingHistogram hist_ptw_translation_latency;             // PTW start -> completed translation
    TimingHistogram hist_lamp_lead_over_ptw;                 // PTW leaf ready cycle - LAMP ready cycle
    TimingHistogram hist_physical_access_advancement;        // Conventional issue cycle - LAMP issue cycle
    TimingHistogram hist_load_completion_latency;            // Load translation miss -> load completion
    TimingHistogram hist_prefetch_completion_rel_validation; // Prefetch completion - validation cycle
    TimingHistogram hist_ifetch_latency;                     // Fetch translation miss -> instruction fetched

    // -------------------------------------------------------------------------
    // D. Translation Latency Aggregates
    // -------------------------------------------------------------------------
    uint64_t load_translation_latency_sum = 0; // Cumulative translation latency for all memory operations
    uint64_t load_translation_count = 0;       // Total count of memory translation events
    uint64_t ptw_queue_wait_cycles = 0;        // Cycles requests spend queued waiting for PTW walker slot

    // -------------------------------------------------------------------------
    // E. Data Access, Contention & Pollution (Side-specific)
    // -------------------------------------------------------------------------
    uint64_t early_data_requests = 0;   // Physical instruction/data requests launched early by LAMP
    uint64_t early_data_l1_hit = 0;     // Early data access hits in L1D cache (when PTW_START_LEVEL == 1)
    uint64_t early_data_l2_hit = 0;     // Early data access hits in L2 cache
    uint64_t early_data_llc_hit = 0;    // Early data access hits in LLC
    uint64_t early_data_dram = 0;       // Early data access reaches off-chip DRAM
    uint64_t early_data_useful = 0;     // Early request successfully served the demand access
    uint64_t early_data_late = 0;       // Early request arrived too late to hide access latency
    uint64_t early_data_duplicate = 0;  // Early request duplicated an existing in-flight MSHR
};

// -----------------------------------------------------------------------------
// LampCoreStats: Core-wide pipeline, stall attribution, and contention metrics
// -----------------------------------------------------------------------------
struct LampCoreStats {
    LampSideStats itlb; // Instruction TLB side statistics
    LampSideStats dtlb; // Data TLB side statistics

    // -------------------------------------------------------------------------
    // D. Core-wide Pipeline & ROB Stall Impacts
    // -------------------------------------------------------------------------
    // ROB stall categories are mutually exclusive per cycle:
    uint64_t rob_head_translation_wait_cycles = 0;          // ROB head stalled waiting for translation only
    uint64_t rob_head_data_wait_cycles = 0;                 // ROB head stalled waiting for demand data only
    uint64_t rob_head_translation_and_data_wait_cycles = 0; // ROB head stalled waiting for both
    uint64_t frontend_starvation_cycles = 0;                // Cycles where fetch buffer was completely empty
    uint64_t ifetch_translation_stall_cycles = 0;           // Fetch stalled waiting for ITLB translation
    uint64_t ifetch_data_stall_cycles = 0;                  // Fetch stalled waiting for L1-I cache fill

    // -------------------------------------------------------------------------
    // Resource Occupancy Sampling (accumulated per cycle across simulation)
    // -------------------------------------------------------------------------
    uint64_t ptw_occupancy_sum = 0;      // Cumulative sum of active PTW walker count
    uint64_t l2_mshr_occupancy_sum = 0;  // Cumulative sum of L2C MSHR occupancy
    uint64_t llc_mshr_occupancy_sum = 0; // Cumulative sum of LLC MSHR occupancy
    uint64_t occupancy_samples = 0;      // Total sampling cycles (used for average occupancy)

    // -------------------------------------------------------------------------
    // E. Extra Probes, Queue Stalls, and Cache Pollution
    // -------------------------------------------------------------------------
    uint64_t lamp_extra_l1_accesses = 0;     // Total probe lookups sent by LAMP into L1D (when PTW_START_LEVEL == 1)
    uint64_t lamp_extra_l2_accesses = 0;     // Total probe lookups sent by LAMP into L2C
    uint64_t lamp_extra_llc_accesses = 0;    // Total probe lookups sent by LAMP into LLC
    uint64_t lamp_queue_stall_cycles = 0;    // Cycles stalled due to L2/LLC read queue full
    uint64_t lamp_mshr_stall_cycles = 0;     // Cycles stalled due to MSHR structural saturation
    uint64_t lamp_pte_evictions = 0;         // Leaf-PTE cache lines evicted from cache hierarchy
    uint64_t lamp_data_evictions = 0;        // Conventional data cache lines displaced by PTEs
    uint64_t lamp_pollution_misses = 0;      // Demand misses induced by PTE displacement of data
    uint64_t lamp_extra_memory_traffic = 0;  // Extra bytes fetched from DRAM due to cache pollution
    std::set<uint64_t> displaced_data_l1d;   // Data block tags displaced from L1D by leaf-PTE fills
    std::set<uint64_t> displaced_data_l2c;   // Data block tags displaced from L2C by leaf-PTE fills
    std::set<uint64_t> displaced_data_llc;   // Data block tags displaced from LLC by leaf-PTE fills
};

// -----------------------------------------------------------------------------
// LampSystem: Central management singleton for the LAMP subsystem
// -----------------------------------------------------------------------------
class LampSystem {
public:
    uint64_t next_trans_id = 1; // Monotonically increasing translation transaction counter
    std::map<uint64_t, TranslationTransaction> active_transactions; // trans_id -> active transaction
    std::map<uint64_t, uint64_t> completed_trans_history;          // trans_id -> completion cycle
    std::vector<LampCoreStats> stats;                              // Per-CPU core statistics array
    std::map<uint64_t, uint64_t> load_miss_start_cycle;            // instr_id -> cycle when load translation miss began
    std::map<uint64_t, uint64_t> ifetch_miss_start_cycle;          // instr_id -> cycle when ifetch translation miss began

    // Lifecycle methods
    void init(uint32_t num_cpus); // Allocates per-core statistics and clears state
    void reset_stats();          // Resets ROI stats upon completion of warmup

    // Leaf cache methods (VA-tagged leaf-PTE cache line organization)
    // PTE_GROUP = VPN >> 3, PTE_OFFSET = VPN & 7
    bool lookup_leaf_cache(uint32_t cpu, uint64_t vpn, uint64_t &ppn, uint8_t &hit_where);
    void fill_leaf_cache(uint32_t cpu, uint64_t vpn, uint64_t ppn, uint8_t source_hit_where);
    void invalidate_leaf_cache(uint32_t cpu, uint64_t vpn);

    // Translation dispatch & competition
    TranslationTransaction* start_transaction(uint32_t cpu, uint64_t instr_id, uint64_t va, uint64_t vpn,
                                             uint64_t ip, uint8_t type, bool is_instruction, uint8_t req_tlb);

    // Miss-to-completion tracking for loads and instruction fetch
    void record_translation_miss_start(uint32_t cpu, uint64_t instr_id, bool is_instruction, uint64_t cycle);
    void record_load_completion(uint32_t cpu, uint64_t instr_id, uint64_t cycle);
    void record_ifetch_completion(uint32_t cpu, uint64_t instr_id, uint64_t cycle);

    // Cancellation & late response safety checks
    bool is_canceled_or_late_response(PACKET *packet);
    void record_late_response_ignored(uint32_t cpu, PACKET *packet);

    // ROB & pipeline stall attribution
    void record_rob_head_stall(uint32_t cpu, bool trans_wait, bool data_wait);
    void record_frontend_starvation(uint32_t cpu, bool empty, bool itlb_wait, bool l1i_wait);
    void record_occupancy(uint32_t cpu, uint32_t l2_occ, uint32_t llc_occ, uint32_t ptw_occ);

    // Output routines
    void print_config(std::ostream &out) const;
    void print_stats(std::ostream &out, uint32_t cpu) const;
    void print_csv_stats(std::ostream &csv, uint32_t cpu) const;
};

// Global singleton instance
extern LampSystem lamp_system;

#endif // LAMP_H
