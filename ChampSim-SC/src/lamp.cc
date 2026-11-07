#include "lamp.h"
#include "cache.h"
#include "ooo_cpu.h"
#include "uncore.h"
#include <cassert>
#include <algorithm>

// Global control knobs defining the simulation configuration:
// knob_lamp: 1 = enable LAMP parallel leaf lookup & request cancellation; 0 = conventional PTW only
uint8_t knob_lamp = DEFAULT_LAMP;
// knob_stlb: 1 = enable Second-Level TLB (STLB); 0 = direct L1-to-memory translation architecture
uint8_t knob_stlb = DEFAULT_STLB;

// Global singleton instance managing translation transactions and stats
LampSystem lamp_system;

// =============================================================================
// TimingHistogram Implementation
// =============================================================================

// print: Outputs formatted latency and lead-time distributions to stdout
// Displays: sample count, arithmetic average, minimum, maximum,
// and two-row vector format ("row1,label1,label2...\nrow2,value1,value2...") for positive and negative bins.
void TimingHistogram::print(std::ostream &out, const std::string &label) const
{
    // Print metric label, total sample count, arithmetic average, min, and max values
    out << "    " << std::left << std::setw(34) << label 
        << " Count: " << std::setw(8) << count
        << " Avg: " << std::fixed << std::setprecision(2) << std::setw(8) << avg()
        << " Min: " << std::setw(6) << (count ? min_val : 0)
        << " Max: " << std::setw(6) << (count ? max_val : 0) << "\n";
    // Print all 9 non-negative cycle latency/lead buckets in 2-row vector format (labels row, then values row)
    out << "      Pos_bins,0,1-4,5-9,10-19,20-39,40-79,80-159,160-319,320+\n";
    out << "      Pos_bins";
    for (int i = 0; i < 9; ++i) out << "," << pos_buckets[i];
    out << "\n";
    // Print all 8 negative cycle lead-time buckets in 2-row vector format (labels row, then values row)
    out << "      Neg_bins,-(1-4),-(5-9),-(10-19),-(20-39),-(40-79),-(80-159),-(160-319),-320+\n";
    out << "      Neg_bins";
    for (int i = 0; i < 8; ++i) out << "," << neg_buckets[i];
    out << "\n";
}

// print_csv: Emits machine-readable CSV rows using standard ChampSim-SC format:
//   Scalars:  "<key_prefix>_avg,<avg>", "<key_prefix>_min,<min>", "<key_prefix>_max,<max>", "<key_prefix>_count,<count>"
//   Histogram vector (two rows with the same key):
//     Row 1 (labels): "<key_prefix>_hist,-320+,-(160-319),-(80-159),-(40-79),-(20-39),-(10-19),-(5-9),-(1-4),0,1-4,5-9,10-19,20-39,40-79,80-159,160-319,320+"
//     Row 2 (values): "<key_prefix>_hist,<neg7>,...,<neg0>,<pos0>,...,<pos8>"
void TimingHistogram::print_csv(std::ostream &csv, const std::string &key_prefix) const
{
    // Summary scalar rows ("key,value")
    csv << key_prefix << "_avg," << std::fixed << std::setprecision(2) << avg() << "\n";
    csv << key_prefix << "_min," << (count ? min_val : 0) << "\n";
    csv << key_prefix << "_max," << (count ? max_val : 0) << "\n";
    csv << key_prefix << "_count," << count << "\n";

    // Two-row histogram vector ("key,label1,label2,...\nkey,value1,value2,...")
    const std::string hist_key = key_prefix + "_hist";
    csv << hist_key
        << ",-320+,-(160-319),-(80-159),-(40-79),-(20-39),-(10-19),-(5-9),-(1-4)"
        << ",0,1-4,5-9,10-19,20-39,40-79,80-159,160-319,320+\n";
    csv << hist_key;
    for (int i = 7; i >= 0; --i) csv << "," << neg_buckets[i]; // Emit negative buckets from -320+ up to -(1-4)
    for (int i = 0; i < 9; ++i) csv << "," << pos_buckets[i];  // Emit positive buckets from 0 up to 320+
    csv << "\n";
}

// =============================================================================
// LampSystem Lifecycle and Setup
// =============================================================================

// init: Initializes data structures for all CPU cores at simulation start
void LampSystem::init(uint32_t num_cpus)
{
    stats.clear();                   // Clear any existing per-core statistics vector
    stats.resize(num_cpus);          // Allocate one LampCoreStats struct per simulated CPU core
    active_transactions.clear();     // Clear in-flight translation transaction table
    completed_trans_history.clear(); // Clear completed transaction history table
    load_miss_start_cycle.clear();   // Clear load translation miss start-cycle map
    ifetch_miss_start_cycle.clear(); // Clear instruction-fetch translation miss start-cycle map
    next_trans_id = 1;               // Reset unique transaction ID generator to 1 (0 is reserved for none)
}

// reset_stats: Clears all accumulated statistics when warmup ends
// Called in finish_warmup() to ensure that warmup phase events do not contaminate ROI metrics.
void LampSystem::reset_stats()
{
    for (auto &cs : stats) {
        cs = LampCoreStats();        // Re-initialize every core's LAMP & baseline statistics to zero
    }
    completed_trans_history.clear(); // Clear warmup transaction completion history
    load_miss_start_cycle.clear();   // Clear warmup in-flight load miss timestamps
    ifetch_miss_start_cycle.clear(); // Clear warmup in-flight instruction-fetch miss timestamps
}

// =============================================================================
// Miss-to-Completion Tracking for Latency Histograms
// =============================================================================

// record_translation_miss_start: Records the cycle timestamp when a translation miss occurs.
// Used to measure end-to-end load completion latency and instruction fetch latency.
void LampSystem::record_translation_miss_start(uint32_t cpu, uint64_t instr_id, bool is_instruction, uint64_t cycle)
{
    (void)cpu;                                  // Timestamp is keyed by globally unique instr_id
    if (is_instruction) {
        ifetch_miss_start_cycle[instr_id] = cycle; // Record cycle when instruction fetch missed in TLB
    } else {
        load_miss_start_cycle[instr_id] = cycle;   // Record cycle when load/store missed in TLB
    }
}

// record_load_completion: Called when a load/store instruction reaches execution completion.
// Computes: load_completion_latency = completion_cycle - translation_miss_start_cycle
// Records the latency in the DTLB hist_load_completion_latency histogram.
void LampSystem::record_load_completion(uint32_t cpu, uint64_t instr_id, uint64_t cycle)
{
    if (cpu >= stats.size()) return;                 // Guard against out-of-range CPU index
    auto it = load_miss_start_cycle.find(instr_id);  // Look up whether this instruction suffered a DTLB/STLB miss
    if (it != load_miss_start_cycle.end()) {
        if (cycle >= it->second) {                   // Ensure monotonic cycle progression
            uint64_t lat = cycle - it->second;       // Compute elapsed cycles from translation miss to data completion
            stats[cpu].dtlb.hist_load_completion_latency.record(lat); // Record sample in DTLB load completion histogram
        }
        load_miss_start_cycle.erase(it);             // Remove entry so merged callbacks do not double-count
    }
}

// record_ifetch_completion: Called when an instruction fetch completes cache fill.
// Computes: ifetch_latency = completion_cycle - translation_miss_start_cycle
// Records the latency in the ITLB hist_ifetch_latency histogram.
void LampSystem::record_ifetch_completion(uint32_t cpu, uint64_t instr_id, uint64_t cycle)
{
    if (cpu >= stats.size()) return;                   // Guard against out-of-range CPU index
    auto it = ifetch_miss_start_cycle.find(instr_id);  // Look up whether this fetch suffered an ITLB/STLB miss
    if (it != ifetch_miss_start_cycle.end()) {
        if (cycle >= it->second) {                     // Ensure monotonic cycle progression
            uint64_t lat = cycle - it->second;         // Compute elapsed cycles from ITLB miss to L1I fetch completion
            stats[cpu].itlb.hist_ifetch_latency.record(lat); // Record sample in ITLB instruction-fetch latency histogram
        }
        ifetch_miss_start_cycle.erase(it);             // Remove entry after recording once
    }
}

// =============================================================================
// Leaf Cache Organization & Lookup (L2 & LLC Only, Never DRAM)
// =============================================================================
// Cache Organization:
//   Assume 4 KiB pages, eight 8-byte PTEs per 64-byte line.
//   VPN = VA >> 12
//   PTE_GROUP = VPN >> 3
//   PTE_OFFSET = VPN & 7
//   Cache indexing uses PTE_GROUP (via unified leaf-PTE line address cl_addr = (base_pt_offset >> 6) + PTE_GROUP).
//   Tag matching uses (CR3/ASID, PTE_GROUP).
//   Both LAMP and the PTW leaf stage share the exact same leaf-PTE cache line in L2 and LLC.
//   Lookup Scope: LAMP probes L2 and LLC only; on a miss at both levels, LAMP stops
//   and never issues a leaf-PTE request to DRAM.

// get_leaf_pte_info: Computes the unified leaf-PTE cache line address and metadata
// matching ChampSim's page table hierarchy so LAMP and PTW leaf walks share the
// exact same cache line without duplicate allocations or address-tag mismatches.
static void get_leaf_pte_info(uint32_t cpu, uint64_t vpn, uint64_t &pte_group, uint32_t &pte_offset,
                              uint64_t &cr3_or_asid, uint64_t &pte_byte_addr, uint64_t &cl_addr, int &leaf_ptw)
{
    pte_group = vpn >> 3;                              // 64-byte leaf-PTE group index (8 contiguous virtual pages per line)
    pte_offset = vpn & 7;                              // Intra-line PTE slot offset (0..7) within the 64-byte cache block
    uint64_t cr3 = 0x200000;                           // Base physical address of the root page table (CR3) in ChampSim
    cr3_or_asid = cr3 + cpu * 0x10000;                 // Per-core address-space identifier (ASID/CR3) for tag matching

    uint64_t ptes_per_page = PAGE_SIZE / 8;            // Number of 8-byte PTEs per page table page (512 for 4 KiB pages)
    uint64_t pte_mask = ptes_per_page - 1;             // Bitmask for extracting per-level index bits (0x1FF for 9-bit index)
    uint64_t log2_ptes = lg2(ptes_per_page);           // Number of VPN bits resolved per page table level (9 bits)
    int walk_depth = (48 - LOG2_PAGE_SIZE + log2_ptes - 1) / log2_ptes; // Total PTW levels (4 levels for 48-bit VA, 4 KiB page)
    leaf_ptw = walk_depth - 1;                         // Leaf PTW level identifier (3 for 4-level page table)

    // Compute the starting byte offset of the leaf-level (lvl == 0) page table region in physical memory,
    // matching va_to_pa() and record_footprint_on_eviction()
    uint64_t base_pt_offset = cr3;
    for (int j = 1; j < walk_depth; j++) {
        uint64_t multiplier = 1;
        for (int k = 0; k < walk_depth - j; k++) multiplier *= ptes_per_page;
        base_pt_offset += multiplier * 8;              // Add size of upper-level page table trees (PML4, PDP, PD)
    }

    // Encode multi-core CPU identifier into high VPN bits if NUM_CPUS > 1, matching va_to_pa()
    uint64_t high_bit_mask = (NUM_CPUS > 1) ? ((uint64_t)cpu << (64 - lg2(NUM_CPUS))) : 0;
    uint64_t vpage = vpn | high_bit_mask;

    // Compute the byte offset of this VPN's 8-byte leaf PTE within the leaf page table region
    uint64_t index_offset = 0;
    for (int j = 0; j < walk_depth; j++) {
        uint64_t idx = (vpage >> (j * log2_ptes)) & pte_mask; // Extract 9-bit VPN index for level j
        uint64_t multiplier = 1;
        for (int k = 0; k < j; k++) multiplier *= ptes_per_page;
        index_offset += idx * multiplier * 8;          // Accumulate byte offset (= vpage * 8)
    }

    pte_byte_addr = base_pt_offset + index_offset;     // Full byte address of the 8-byte leaf PTE
    cl_addr = pte_byte_addr >> LOG2_BLOCK_SIZE;        // 64-byte cache line address shared by all 8 sibling VPNs in pte_group
}

// lookup_leaf_cache: Probes L1D (if PTW_START_LEVEL == 1), L2, and LLC for the leaf PTE line
// Parameters:
//   cpu: originating core ID
//   vpn: virtual page number
//   ppn: output parameter receiving physical page number if found
//   hit_where: output parameter receiving cache level (1 = L1D, 2 = L2, 3 = LLC, 0xFF = Unusable Hit, 0 = Miss)
// Returns true on usable hit; false on miss or unusable hit.
bool LampSystem::lookup_leaf_cache(uint32_t cpu, uint64_t vpn, uint64_t &ppn, uint8_t &hit_where)
{
    uint64_t pte_group = 0, cr3 = 0, pte_byte_addr = 0, cl_addr = 0;
    uint32_t pte_offset = 0;
    int leaf_ptw = 3;
    // Derive unified leaf-PTE cache line address, VA group tag (VPN >> 3), and intra-line offset (VPN & 7)
    get_leaf_pte_info(cpu, vpn, pte_group, pte_offset, cr3, pte_byte_addr, cl_addr, leaf_ptw);

    // 0. If PTW starts from L1 (PTW_START_LEVEL == 1), probe L1D Cache first for a matching VA-tagged leaf-PTE block
    if (PTW_START_LEVEL == 1) {
        stats[cpu].lamp_extra_l1_accesses++;               // Count this LAMP probe access sent to the L1D cache
        CACHE &l1 = ooo_cpu[cpu].L1D;
        uint32_t l1_set = l1.get_set(cl_addr);             // Index L1D cache set using the leaf-PTE cache line address
        for (uint32_t w = 0; w < l1.NUM_WAY; ++w) {
            if (l1.block[l1_set][w].valid &&               // Block must be valid
                l1.block[l1_set][w].tag == cl_addr &&      // Cache line tag must match leaf-PTE block address
                l1.block[l1_set][w].is_leaf_pte &&         // Block must be marked as a leaf-PTE line
                l1.block[l1_set][w].pte_group == pte_group && // VA group tag (VPN >> 3) must match
                l1.block[l1_set][w].cr3_or_asid == cr3) {  // Address space identifier (CR3/ASID) must match
                // Check whether the requested PTE slot (pte_offset = VPN & 7) is valid in the 8-bit bitmap
                if (l1.block[l1_set][w].leaf_pte_valid & (1 << pte_offset)) {
                    if (l1.block[l1_set][w].leaf_ptes[pte_offset] != 0) {
                        ppn = l1.block[l1_set][w].leaf_ptes[pte_offset]; // Read cached PPN from the PTE slot
                    } else if (lookup_allocated_pte(cpu, vpn, &ppn)) {
                        l1.block[l1_set][w].leaf_ptes[pte_offset] = ppn; // Populate sibling PPN from page_table if already mapped
                    } else {
                        ppn = 0; // First touch of this sibling page in a cached 64B leaf-PTE line; PPN assigned by OS/PTW
                    }
                    hit_where = 1; // Report LAMP hit in L1D (hit_where == 1)
                    return true;   // Usable leaf-PTE hit in L1D
                } else {
                    // The 64-byte leaf-PTE line is resident in L1D, but this specific PTE slot was invalidated
                    hit_where = 0xFF; // Report unusable LAMP hit
                    return false;     // Cannot complete translation from LAMP
                }
            }
        }
    }

    // Count this LAMP probe access sent to the L2 cache
    stats[cpu].lamp_extra_l2_accesses++;

    // 1. Probe L2 Cache for a matching VA-tagged leaf-PTE block
    CACHE &l2 = ooo_cpu[cpu].L2C;
    uint32_t l2_set = l2.get_set(cl_addr);             // Index L2 cache set using the leaf-PTE cache line address
    for (uint32_t w = 0; w < l2.NUM_WAY; ++w) {
        if (l2.block[l2_set][w].valid &&               // Block must be valid
            l2.block[l2_set][w].tag == cl_addr &&      // Cache line tag must match leaf-PTE block address
            l2.block[l2_set][w].is_leaf_pte &&         // Block must be marked as a leaf-PTE line
            l2.block[l2_set][w].pte_group == pte_group && // VA group tag (VPN >> 3) must match
            l2.block[l2_set][w].cr3_or_asid == cr3) {  // Address space identifier (CR3/ASID) must match
            // Check whether the requested PTE slot (pte_offset = VPN & 7) is valid in the 8-bit bitmap
            if (l2.block[l2_set][w].leaf_pte_valid & (1 << pte_offset)) {
                if (l2.block[l2_set][w].leaf_ptes[pte_offset] != 0) {
                    ppn = l2.block[l2_set][w].leaf_ptes[pte_offset]; // Read cached PPN from the PTE slot
                } else if (lookup_allocated_pte(cpu, vpn, &ppn)) {
                    l2.block[l2_set][w].leaf_ptes[pte_offset] = ppn; // Populate sibling PPN from page_table if already mapped
                } else {
                    ppn = 0; // First touch of this sibling page in a cached 64B leaf-PTE line; PPN assigned by OS/PTW
                }
                hit_where = 2; // Report LAMP hit in L2C (hit_where == 2)
                return true;   // Usable leaf-PTE hit in L2C
            } else {
                // The 64-byte leaf-PTE line is resident in L2, but this specific PTE slot was invalidated (e.g., page swap)
                hit_where = 0xFF; // Report unusable LAMP hit
                return false;     // Cannot complete translation from LAMP
            }
        }
    }

    // L2 missed: count the subsequent LAMP probe access sent to the LLC
    stats[cpu].lamp_extra_llc_accesses++;

    // 2. Probe Last-Level Cache (LLC) for a matching VA-tagged leaf-PTE block
    CACHE &llc = uncore.LLC;
    uint32_t llc_set = llc.get_set(cl_addr);           // Index LLC set using the leaf-PTE cache line address
    for (uint32_t w = 0; w < llc.NUM_WAY; ++w) {
        if (llc.block[llc_set][w].valid &&             // Block must be valid
            llc.block[llc_set][w].tag == cl_addr &&    // Cache line tag must match leaf-PTE block address
            llc.block[llc_set][w].is_leaf_pte &&       // Block must be marked as a leaf-PTE line
            llc.block[llc_set][w].pte_group == pte_group && // VA group tag (VPN >> 3) must match
            llc.block[llc_set][w].cr3_or_asid == cr3) {// Address space identifier (CR3/ASID) must match
            // Check whether the requested PTE slot (pte_offset = VPN & 7) is valid in the 8-bit bitmap
            if (llc.block[llc_set][w].leaf_pte_valid & (1 << pte_offset)) {
                if (llc.block[llc_set][w].leaf_ptes[pte_offset] != 0) {
                    ppn = llc.block[llc_set][w].leaf_ptes[pte_offset]; // Read cached PPN from the PTE slot
                } else if (lookup_allocated_pte(cpu, vpn, &ppn)) {
                    llc.block[llc_set][w].leaf_ptes[pte_offset] = ppn; // Populate sibling PPN from page_table if already mapped
                } else {
                    ppn = 0; // First touch of this sibling page in a cached 64B leaf-PTE line; PPN assigned by OS/PTW
                }
                hit_where = 3; // Report LAMP hit in LLC (hit_where == 3)
                return true;   // Usable leaf-PTE hit in LLC
            } else {
                // The 64-byte leaf-PTE line is resident in LLC, but this specific PTE slot was invalidated
                hit_where = 0xFF; // Report unusable LAMP hit
                return false;     // Cannot complete translation from LAMP
            }
        }
    }

    // 3. Absent from searched caches (L1D/L2/LLC): LAMP stops immediately and NEVER issues a request to DRAM
    hit_where = 0; // Report LAMP miss
    return false;
}

// fill_leaf_cache: Attaches VA-tagged leaf PTE metadata to the unified leaf PTE cache line
// in L1D (when PTW_START_LEVEL == 1), L2, and LLC that was installed by the page table walker.
void LampSystem::fill_leaf_cache(uint32_t cpu, uint64_t vpn, uint64_t ppn, uint8_t source_hit_where)
{
    (void)source_hit_where;
    uint64_t pte_group = 0, cr3 = 0, pte_byte_addr = 0, cl_addr = 0;
    uint32_t pte_offset = 0;
    int leaf_ptw = 3;
    // Compute the unified cache line address and VA group metadata for this VPN
    get_leaf_pte_info(cpu, vpn, pte_group, pte_offset, cr3, pte_byte_addr, cl_addr, leaf_ptw);

    // 1. Update the leaf PTE cache block in LLC if resident
    CACHE &llc = uncore.LLC;
    uint32_t llc_set = llc.get_set(cl_addr);           // Locate LLC set where va_to_pa installed the leaf PTE line
    for (uint32_t w = 0; w < llc.NUM_WAY; ++w) {
        if (llc.block[llc_set][w].valid && llc.block[llc_set][w].tag == cl_addr) {
            // If this cache block is newly filled as a leaf-PTE block or changed group, initialize its LAMP fields
            if (!llc.block[llc_set][w].is_leaf_pte || llc.block[llc_set][w].pte_group != pte_group) {
                llc.block[llc_set][w].is_leaf_pte = 1;        // Flag block as holding a VA-tagged group of 8 leaf PTEs
                llc.block[llc_set][w].pte_group = pte_group;  // Record virtual page group tag (VPN >> 3)
                llc.block[llc_set][w].cr3_or_asid = cr3;      // Record address space identifier
                llc.block[llc_set][w].leaf_pte_valid = 0xFF;  // All 8 PTE slots in this fetched 64B leaf PT line are valid
                for (int i = 0; i < 8; ++i) llc.block[llc_set][w].leaf_ptes[i] = 0; // Clear PPN array before populating
            }
            llc.block[llc_set][w].leaf_ptes[pte_offset] = ppn;        // Store translated PPN at slot (VPN & 7)
            llc.block[llc_set][w].leaf_pte_valid |= (1 << pte_offset);// Ensure valid bit for slot (VPN & 7) is set

            // Populate any sibling PTEs (offsets 0..7 in the same 64-byte line) that already have OS page_table mappings
            for (uint32_t off = 0; off < 8; ++off) {
                uint64_t sibling_ppn = 0;
                if (lookup_allocated_pte(cpu, (pte_group << 3) + off, &sibling_ppn)) {
                    llc.block[llc_set][w].leaf_ptes[off] = sibling_ppn;
                }
            }
            break; // Found and updated the unique matching LLC block
        }
    }

    // 2. Update the leaf PTE cache block in L2C if resident
    CACHE &l2 = ooo_cpu[cpu].L2C;
    uint32_t l2_set = l2.get_set(cl_addr);             // Locate L2C set where va_to_pa installed the leaf PTE line
    for (uint32_t w = 0; w < l2.NUM_WAY; ++w) {
        if (l2.block[l2_set][w].valid && l2.block[l2_set][w].tag == cl_addr) {
            // If this cache block is newly filled as a leaf-PTE block or changed group, initialize its LAMP fields
            if (!l2.block[l2_set][w].is_leaf_pte || l2.block[l2_set][w].pte_group != pte_group) {
                l2.block[l2_set][w].is_leaf_pte = 1;          // Flag block as holding a VA-tagged group of 8 leaf PTEs
                l2.block[l2_set][w].pte_group = pte_group;    // Record virtual page group tag (VPN >> 3)
                l2.block[l2_set][w].cr3_or_asid = cr3;        // Record address space identifier
                l2.block[l2_set][w].leaf_pte_valid = 0xFF;    // All 8 PTE slots in this fetched 64B leaf PT line are valid
                for (int i = 0; i < 8; ++i) l2.block[l2_set][w].leaf_ptes[i] = 0; // Clear PPN array before populating
            }
            l2.block[l2_set][w].leaf_ptes[pte_offset] = ppn;          // Store translated PPN at slot (VPN & 7)
            l2.block[l2_set][w].leaf_pte_valid |= (1 << pte_offset);  // Ensure valid bit for slot (VPN & 7) is set

            // Populate any sibling PTEs (offsets 0..7 in the same 64-byte line) that already have OS page_table mappings
            for (uint32_t off = 0; off < 8; ++off) {
                uint64_t sibling_ppn = 0;
                if (lookup_allocated_pte(cpu, (pte_group << 3) + off, &sibling_ppn)) {
                    l2.block[l2_set][w].leaf_ptes[off] = sibling_ppn;
                }
            }
            break; // Found and updated the unique matching L2C block
        }
    }

    // 3. If PTW starts from L1 (PTW_START_LEVEL == 1), update the leaf PTE cache block in L1D if resident
    if (PTW_START_LEVEL == 1) {
        CACHE &l1 = ooo_cpu[cpu].L1D;
        uint32_t l1_set = l1.get_set(cl_addr);         // Locate L1D set where va_to_pa installed the leaf PTE line
        for (uint32_t w = 0; w < l1.NUM_WAY; ++w) {
            if (l1.block[l1_set][w].valid && l1.block[l1_set][w].tag == cl_addr) {
                // If this cache block is newly filled as a leaf-PTE block or changed group, initialize its LAMP fields
                if (!l1.block[l1_set][w].is_leaf_pte || l1.block[l1_set][w].pte_group != pte_group) {
                    l1.block[l1_set][w].is_leaf_pte = 1;      // Flag block as holding a VA-tagged group of 8 leaf PTEs
                    l1.block[l1_set][w].pte_group = pte_group;// Record virtual page group tag (VPN >> 3)
                    l1.block[l1_set][w].cr3_or_asid = cr3;    // Record address space identifier
                    l1.block[l1_set][w].leaf_pte_valid = 0xFF;// All 8 PTE slots in this fetched 64B leaf PT line are valid
                    for (int i = 0; i < 8; ++i) l1.block[l1_set][w].leaf_ptes[i] = 0; // Clear PPN array before populating
                }
                l1.block[l1_set][w].leaf_ptes[pte_offset] = ppn;          // Store translated PPN at slot (VPN & 7)
                l1.block[l1_set][w].leaf_pte_valid |= (1 << pte_offset);  // Ensure valid bit for slot (VPN & 7) is set

                // Populate any sibling PTEs (offsets 0..7 in the same 64-byte line) that already have OS page_table mappings
                for (uint32_t off = 0; off < 8; ++off) {
                    uint64_t sibling_ppn = 0;
                    if (lookup_allocated_pte(cpu, (pte_group << 3) + off, &sibling_ppn)) {
                        l1.block[l1_set][w].leaf_ptes[off] = sibling_ppn;
                    }
                }
                break; // Found and updated the unique matching L1D block
            }
        }
    }
}

// invalidate_leaf_cache: Handles page unmapping or context switches
// Invalidates the PTE entry from L1D (when PTW_START_LEVEL == 1), L2, and LLC cache lines.
void LampSystem::invalidate_leaf_cache(uint32_t cpu, uint64_t vpn)
{
    uint64_t pte_group = 0, cr3 = 0, pte_byte_addr = 0, cl_addr = 0;
    uint32_t pte_offset = 0;
    int leaf_ptw = 3;
    // Compute the unified cache line address and intra-line offset for the unmapped VPN
    get_leaf_pte_info(cpu, vpn, pte_group, pte_offset, cr3, pte_byte_addr, cl_addr, leaf_ptw);

    // Invalidate the specific PTE slot in L1D if PTW_START_LEVEL == 1 and the leaf-PTE block is present
    if (PTW_START_LEVEL == 1) {
        CACHE &l1 = ooo_cpu[cpu].L1D;
        uint32_t l1_set = l1.get_set(cl_addr);
        for (uint32_t w = 0; w < l1.NUM_WAY; ++w) {
            if (l1.block[l1_set][w].valid && l1.block[l1_set][w].tag == cl_addr &&
                l1.block[l1_set][w].is_leaf_pte &&
                l1.block[l1_set][w].pte_group == pte_group && l1.block[l1_set][w].cr3_or_asid == cr3) {
                l1.block[l1_set][w].leaf_pte_valid &= ~(1 << pte_offset); // Clear valid bit for the unmapped PTE slot
                l1.block[l1_set][w].leaf_ptes[pte_offset] = 0;            // Clear stored PPN for the unmapped PTE slot
                if (l1.block[l1_set][w].leaf_pte_valid == 0) {            // If all 8 PTE slots in the line are now invalid:
                    l1.block[l1_set][w].valid = 0;                        // Invalidate the entire L1D cache block
                    l1.block[l1_set][w].is_leaf_pte = 0;                  // Clear leaf-PTE flag
                }
            }
        }
    }

    // Invalidate the specific PTE slot in L2C if the leaf-PTE block is present
    CACHE &l2 = ooo_cpu[cpu].L2C;
    uint32_t l2_set = l2.get_set(cl_addr);
    for (uint32_t w = 0; w < l2.NUM_WAY; ++w) {
        if (l2.block[l2_set][w].valid && l2.block[l2_set][w].tag == cl_addr &&
            l2.block[l2_set][w].is_leaf_pte &&
            l2.block[l2_set][w].pte_group == pte_group && l2.block[l2_set][w].cr3_or_asid == cr3) {
            l2.block[l2_set][w].leaf_pte_valid &= ~(1 << pte_offset); // Clear valid bit for the unmapped PTE slot
            l2.block[l2_set][w].leaf_ptes[pte_offset] = 0;            // Clear stored PPN for the unmapped PTE slot
            if (l2.block[l2_set][w].leaf_pte_valid == 0) {            // If all 8 PTE slots in the line are now invalid:
                l2.block[l2_set][w].valid = 0;                        // Invalidate the entire L2 cache block
                l2.block[l2_set][w].is_leaf_pte = 0;                  // Clear leaf-PTE flag
            }
        }
    }

    // Invalidate the specific PTE slot in LLC if the leaf-PTE block is present
    CACHE &llc = uncore.LLC;
    uint32_t llc_set = llc.get_set(cl_addr);
    for (uint32_t w = 0; w < llc.NUM_WAY; ++w) {
        if (llc.block[llc_set][w].valid && llc.block[llc_set][w].tag == cl_addr &&
            llc.block[llc_set][w].is_leaf_pte &&
            llc.block[llc_set][w].pte_group == pte_group && llc.block[llc_set][w].cr3_or_asid == cr3) {
            llc.block[llc_set][w].leaf_pte_valid &= ~(1 << pte_offset); // Clear valid bit for the unmapped PTE slot
            llc.block[llc_set][w].leaf_ptes[pte_offset] = 0;            // Clear stored PPN for the unmapped PTE slot
            if (llc.block[llc_set][w].leaf_pte_valid == 0) {            // If all 8 PTE slots in the line are now invalid:
                llc.block[llc_set][w].valid = 0;                        // Invalidate the entire LLC cache block
                llc.block[llc_set][w].is_leaf_pte = 0;                  // Clear leaf-PTE flag
            }
        }
    }
}

// =============================================================================
// Translation Transaction Dispatch & Competition
// =============================================================================

// start_transaction: Instantiates a shared translation transaction tracking both paths
TranslationTransaction* LampSystem::start_transaction(uint32_t cpu, uint64_t instr_id, uint64_t va, uint64_t vpn,
                                                      uint64_t ip, uint8_t type, bool is_instruction, uint8_t req_tlb)
{
    uint64_t id = next_trans_id++;               // Allocate a globally unique 64-bit transaction ID
    TranslationTransaction tx;                   // Instantiate a new TranslationTransaction record
    tx.trans_id = id;                            // Bind unique transaction ID
    tx.cpu = cpu;                                // Record originating CPU core ID
    tx.instr_id = instr_id;                      // Record originating pipeline instruction ID
    tx.va = va;                                  // Record full 64-bit virtual address
    tx.vpn = vpn;                                // Record virtual page number (VA >> 12)
    tx.ip = ip;                                  // Record instruction pointer
    tx.type = type;                              // Record memory access type (LOAD, RFO, PREFETCH)
    tx.is_instruction = is_instruction;          // Flag true if ITLB request, false if DTLB request
    tx.requesting_tlb = req_tlb;                 // Record cache_type of requesting TLB (IS_ITLB, IS_DTLB, IS_STLB)
    tx.state = STATE_PENDING;                    // Initialize transaction state machine to STATE_PENDING
    tx.trigger_cycle = current_core_cycle[cpu];  // Record core cycle when the translation race started

    active_transactions[id] = tx;                // Insert transaction into active transactions table
    return &active_transactions[id];             // Return pointer to the stored transaction
}

// is_canceled_or_late_response: Inspects whether an arriving response corresponds to
// an already-completed transaction or a canceled path.
// Critical for preventing double MSHR releases or assertion failures in return_data().
bool LampSystem::is_canceled_or_late_response(PACKET *packet)
{
    if (!packet) return false;                   // Null packet guard
    if (packet->canceled || packet->is_losing_path) return true; // Packet was explicitly marked canceled or losing path
    if (packet->trans_id != 0) {                 // If packet is associated with a LAMP/PTW transaction ID:
        auto it = completed_trans_history.find(packet->trans_id); // Check if the transaction already completed
        if (it != completed_trans_history.end()) {
            return true;                         // Transaction was already satisfied by the winning path
        }
    }
    return false;                                // Packet belongs to an active, uncompleted request
}

// record_late_response_ignored: Accounting for canceled or late packets arriving at cache callbacks
void LampSystem::record_late_response_ignored(uint32_t cpu, PACKET *packet)
{
    if (cpu < stats.size()) {
        bool is_itlb = (packet && packet->instruction);            // Determine whether packet belongs to ITLB or DTLB side
        LampSideStats &s = is_itlb ? stats[cpu].itlb : stats[cpu].dtlb; // Select corresponding side stats struct
        s.late_responses_ignored++;                                // Increment count of safely discarded late responses
        if (packet && packet->is_lamp)
            s.lamp_late_hit++;                                     // Attribute late callback to LAMP path
        else
            s.ptw_late_response++;                                 // Attribute late callback to PTW path
    }
}

// =============================================================================
// Pipeline & ROB Stall Attribution
// =============================================================================

// record_rob_head_stall: Samples the head of the ROB each cycle
// Mutually exclusive stall attribution:
//   - trans_wait && !data_wait -> rob_head_translation_wait_cycles++
//   - !trans_wait && data_wait -> rob_head_data_wait_cycles++
//   - trans_wait && data_wait  -> rob_head_translation_and_data_wait_cycles++
void LampSystem::record_rob_head_stall(uint32_t cpu, bool trans_wait, bool data_wait)
{
    if (cpu < stats.size()) {
        if (trans_wait && !data_wait)
            stats[cpu].rob_head_translation_wait_cycles++;          // ROB head blocked strictly on address translation
        else if (!trans_wait && data_wait)
            stats[cpu].rob_head_data_wait_cycles++;                 // ROB head blocked strictly on demand data fetch
        else if (trans_wait && data_wait)
            stats[cpu].rob_head_translation_and_data_wait_cycles++; // ROB head blocked on both translation and data
    }
}

// record_frontend_starvation: Tracks frontend instruction fetch starvation and stalls
void LampSystem::record_frontend_starvation(uint32_t cpu, bool empty, bool itlb_wait, bool l1i_wait)
{
    if (cpu < stats.size()) {
        if (empty)
            stats[cpu].frontend_starvation_cycles++;      // Increment cycles where IFETCH_BUFFER is completely empty
        if (itlb_wait)
            stats[cpu].ifetch_translation_stall_cycles++; // Increment cycles where oldest fetch entry waits on ITLB
        if (l1i_wait)
            stats[cpu].ifetch_data_stall_cycles++;        // Increment cycles where oldest fetch entry waits on L1I
    }
}

// record_occupancy: Accumulates occupancy samples per cycle for average queue utilization
void LampSystem::record_occupancy(uint32_t cpu, uint32_t l2_occ, uint32_t llc_occ, uint32_t ptw_occ)
{
    if (cpu < stats.size()) {
        stats[cpu].l2_mshr_occupancy_sum += l2_occ;   // Add current cycle's L2C MSHR occupancy to running sum
        stats[cpu].llc_mshr_occupancy_sum += llc_occ; // Add current cycle's LLC MSHR occupancy to running sum
        stats[cpu].ptw_occupancy_sum += ptw_occ;      // Add current cycle's active PTW walker count to running sum
        stats[cpu].occupancy_samples++;               // Increment total number of sampled core cycles
    }
}

// =============================================================================
// Formatted Output Display
// =============================================================================

// Helper: Computes total L1 TLB misses for ITLB or DTLB on the given CPU core
static uint64_t get_l1_tlb_misses(uint32_t cpu, bool is_itlb, uint64_t fallback_walks)
{
    CACHE &tlb = is_itlb ? ooo_cpu[cpu].ITLB : ooo_cpu[cpu].DTLB;
    uint64_t total_miss = 0;
    for (uint32_t t = 0; t < NUM_TYPES; ++t) {
        total_miss += tlb.sim_miss[cpu][t];
    }
    return (total_miss > 0) ? total_miss : fallback_walks;
}

// Helper: Formats concise per-side summary (ITLB or DTLB) for console display
static void print_side_summary(std::ostream &out, uint32_t cpu, const std::string &side, const LampSideStats &s)
{
    bool is_itlb = (side == "ITLB");
    uint64_t l1_tlb_misses = get_l1_tlb_misses(cpu, is_itlb, s.load_translation_count);
    // Average walk latency on a last-level TLB miss (STLB miss when knob_stlb=1, L1 TLB miss when knob_stlb=0)
    double avg_walk_lat = s.load_translation_count ? (double)s.load_translation_latency_sum / s.load_translation_count : 0.0;
    // Effective L1 TLB miss service latency (accounts for STLB hit/miss latency when knob_stlb=1, or direct walk when knob_stlb=0)
    double avg_tlb_miss_service_lat = 0.0;
    if (knob_stlb) {
        avg_tlb_miss_service_lat = l1_tlb_misses ? (double)(l1_tlb_misses * STLB_LATENCY + s.load_translation_latency_sum) / l1_tlb_misses : 0.0;
    } else {
        avg_tlb_miss_service_lat = avg_walk_lat;
    }

    out << "\n  [" << side << " Summary]\n";
    // 1. TLB Filled by LAMP or PTW Count
    out << "    TLB Fills (LAMP vs PTW):\n"
        << "      tlb_fill_source,lamp,ptw,total\n"
        << "      tlb_fill_source," << s.lamp_tlb_fills << "," << s.ptw_tlb_fills << "," << s.triggers << "\n";

    // 2. Hit Where LAMP
    out << "    LAMP Hit Where:\n"
        << "      lamp_hit_where,l1,l2,llc,miss\n"
        << "      lamp_hit_where," << s.lamp_l1_hit << "," << s.lamp_l2_hit << ","
        << s.lamp_llc_hit << "," << s.lamp_miss << "\n";

    // 3. Walks Canceled by LAMP
    out << "    Walks Canceled by LAMP: " << s.ptw_cancel_count
        << " (PTW Requests Avoided: " << s.ptw_requests_avoided << ")\n"
        << "      ptw_cancel_stage,PML4,PDP,PD,Leaf\n"
        << "      ptw_cancel_stage," << s.ptw_cancel_pml4 << "," << s.ptw_cancel_pdp << ","
        << s.ptw_cancel_pd << "," << s.ptw_cancel_leaf << "\n";

    // 4. TLB Miss Service Latencies (works with or without STLB)
    out << "    TLB Miss Service Latencies:\n"
        << "      Effective L1 " << side << " Miss Service Latency: " << std::fixed << std::setprecision(2)
        << avg_tlb_miss_service_lat << " cycles (L1 TLB Misses: " << l1_tlb_misses
        << ", STLB: " << (knob_stlb ? "ON" : "OFF") << ")\n"
        << "      Last-Level TLB Miss Walk Latency:       " << std::fixed << std::setprecision(2)
        << avg_walk_lat << " cycles (Walks: " << s.load_translation_count << ")\n"
        << "      LAMP Hit Latency Avg:                   " << std::fixed << std::setprecision(2)
        << s.hist_lamp_lookup_latency.avg() << " cycles\n"
        << "      Un-canceled PTW Walk Latency Avg:       " << std::fixed << std::setprecision(2)
        << s.hist_ptw_translation_latency.avg() << " cycles\n";
}

// print_config: Displays active LAMP and translation system parameters to stdout
void LampSystem::print_config(std::ostream &out) const
{
    // Map (knob_stlb, knob_lamp) into canonical configuration identifier: B0, B1, L0, or L1
    std::string config_name = knob_lamp ? (knob_stlb ? "L1" : "L0") : (knob_stlb ? "B1" : "B0");
    out << "\n=== LAMP & TRANSLATION SETTINGS ===\n";
    out << "  Configuration ID: " << config_name << " [";
    if (knob_lamp && knob_stlb) out << "STLB ON, LAMP ON (L1)]\n";             // L1: STLB enabled + LAMP enabled
    else if (knob_lamp && !knob_stlb) out << "STLB OFF, LAMP ON (L0)]\n";      // L0: STLB disabled + LAMP enabled
    else if (!knob_lamp && knob_stlb) out << "STLB ON, LAMP OFF (B1 Baseline)]\n"; // B1: STLB enabled + LAMP disabled
    else out << "STLB OFF, LAMP OFF (B0 Baseline)]\n";                         // B0: STLB disabled + LAMP disabled

    out << "  LAMP Enabled:     " << (knob_lamp ? "YES (1)" : "NO (0)") << "\n"; // Print active knob_lamp state
    out << "  STLB Enabled:     " << (knob_stlb ? "YES (1)" : "NO (0)") << "\n"; // Print active knob_stlb state
    out << "  PTW Start Level:  " << (PTW_START_LEVEL == 1 ? "L1D Cache (1)" : "L2 Cache (2)") << "\n"; // Print active PTW_START_LEVEL
    if (knob_lamp) {
        // Print architectural parameters of the LAMP parallel leaf-PTE lookup engine
        out << "  Lookup Levels:    " << (PTW_START_LEVEL == 1 ? "L1D Cache, L2 Cache, LLC Cache" : "L2 Cache, LLC Cache") << " (DRAM lookup: Prohibited)\n";
        out << "  PTE Group Size:   8 PTEs / 64-byte line (4 KiB pages, 8-byte PTEs)\n";
        out << "  Tag Matching:     (ASID, PTE_GROUP) with PTE_OFFSET = VPN & 7\n";
        out << "  Trigger Point:    " << (knob_stlb ? "STLB miss (races with PTW)" : "L1 ITLB/DTLB miss (races with PTW)") << "\n";
        out << "  Cancellation:     Eager loser cancellation with ref-counted MSHR safe release\n";
    } else {
        // Print baseline page table walker parameters when LAMP is disabled
        out << "  Translation Path: Conventional Page Table Walker (PTW)\n";
        out << "  Trigger Point:    " << (knob_stlb ? "STLB miss -> PTW" : "L1 ITLB/DTLB miss -> PTW") << "\n";
    }
    out << "===================================\n\n";
}

// print_stats: Emits concise human-readable ROI performance report to stdout
void LampSystem::print_stats(std::ostream &out, uint32_t cpu) const
{
    if (cpu >= stats.size()) return;                                           // Guard against invalid CPU core index
    const LampCoreStats &cs = stats[cpu];                                      // Retrieve per-core LAMP/baseline statistics
    std::string config_name = knob_lamp ? (knob_stlb ? "L1" : "L0") : (knob_stlb ? "B1" : "B0");

    out << "\n======================================================\n";
    if (knob_lamp) {
        out << "  LAMP SUMMARY STATS (Core " << cpu << ")\n";
        out << "  Configuration: " << config_name << " [STLB: " << (knob_stlb ? "ON" : "OFF") << ", LAMP: ON]\n";
    } else {
        out << "  BASELINE TRANSLATION SUMMARY STATS (Core " << cpu << ")\n";
        out << "  Configuration: " << config_name << " [STLB: " << (knob_stlb ? "ON" : "OFF") << ", LAMP: OFF]\n";
    }
    out << "======================================================\n";

    print_side_summary(out, cpu, "ITLB", cs.itlb);
    print_side_summary(out, cpu, "DTLB", cs.dtlb);

    // Cache Pollution by Leaf PTEs
    out << "\n  [Cache Pollution by Leaf PTEs]\n"
        << "    Data Lines Displaced by Leaf PTEs:      " << cs.lamp_data_evictions << "\n"
        << "    Leaf-PTE Lines Evicted from Cache:      " << cs.lamp_pte_evictions << "\n"
        << "    Demand Misses from Leaf-PTE Pollution:  " << cs.lamp_pollution_misses << "\n";

    // Core-wide ROB & Pipeline Stall Metrics
    out << "\n  [ROB & Pipeline Stall Metrics]\n"
        << "    ROB-Head Waiting for Translation Only:  " << cs.rob_head_translation_wait_cycles << "\n"
        << "    ROB-Head Waiting for Demand Data Only:  " << cs.rob_head_data_wait_cycles << "\n"
        << "    ROB-Head Waiting for Both Trans & Data: " << cs.rob_head_translation_and_data_wait_cycles << "\n"
        << "    Frontend Starvation Cycles:             " << cs.frontend_starvation_cycles << "\n"
        << "    IFetch Translation Stall Cycles:        " << cs.ifetch_translation_stall_cycles << "\n"
        << "    IFetch L1-I Data Stall Cycles:          " << cs.ifetch_data_stall_cycles << "\n";
    out << "======================================================\n\n";
}

// print_csv_stats: Emits only the essential LAMP & translation CSV metrics in standard ChampSim-SC format:
//   - Scalar metrics: "key,value\n"
//   - Vector metrics: two rows with the same key:
//       "key,label1,label2,...\n"
//       "key,value1,value2,...\n"
void LampSystem::print_csv_stats(std::ostream &csv, uint32_t cpu) const
{
    if (cpu >= stats.size()) return;                                           // Guard against invalid CPU core index
    const LampCoreStats &cs = stats[cpu];                                      // Retrieve per-core LAMP/baseline statistics
    const std::string core_prefix = "Core_" + std::to_string(cpu) + "_";       // Standard per-core CSV key prefix
    std::string config_name = knob_lamp ? (knob_stlb ? "L1" : "L0") : (knob_stlb ? "B1" : "B0");

    // Helper lambda to emit an integer scalar "key,value" row
    auto write_scalar = [&](const std::string &key, uint64_t val) {
        csv << key << "," << val << "\n";
    };

    // Helper lambda to emit a floating-point scalar "key,value" row
    auto write_double = [&](const std::string &key, double val) {
        csv << key << "," << std::fixed << std::setprecision(2) << val << "\n";
    };

    // Helper lambda to emit a two-row vector ("key,label1,label2,...\nkey,val1,val2,...\n")
    auto write_vector = [&](const std::string &key, const std::vector<std::string> &labels, const std::vector<uint64_t> &values) {
        csv << key;
        for (const auto &l : labels) csv << "," << l;
        csv << "\n" << key;
        for (const auto &v : values) csv << "," << v;
        csv << "\n";
    };

    // 1. Configuration scalars
    csv << core_prefix << "lamp_config," << config_name << "\n";               // Canonical config ID: B0, B1, L0, or L1
    write_scalar(core_prefix + "lamp_enabled", (uint32_t)knob_lamp);           // 1 = LAMP ON, 0 = LAMP OFF
    write_scalar(core_prefix + "stlb_enabled", (uint32_t)knob_stlb);           // 1 = STLB ON, 0 = STLB OFF
    write_scalar(core_prefix + "ptw_start_level", (uint32_t)PTW_START_LEVEL);  // 1 = L1D, 2 = L2C

    // Lambda to emit essential per-side (ITLB / DTLB) metrics
    auto write_side_csv = [&](const std::string &side, bool is_itlb, const LampSideStats &s) {
        const std::string p = core_prefix + side + "_";                        // Per-side prefix: Core_<cpu>_<ITLB|DTLB>_
        uint64_t l1_tlb_misses = get_l1_tlb_misses(cpu, is_itlb, s.load_translation_count);
        double avg_walk_lat = s.load_translation_count ? (double)s.load_translation_latency_sum / s.load_translation_count : 0.0;
        double avg_tlb_miss_service_lat = knob_stlb
            ? (l1_tlb_misses ? (double)(l1_tlb_misses * STLB_LATENCY + s.load_translation_latency_sum) / l1_tlb_misses : 0.0)
            : avg_walk_lat;

        // A. TLB Filled by LAMP or PTW Count
        write_scalar(p + "tlb_filled_by_lamp", s.lamp_tlb_fills);
        write_scalar(p + "tlb_filled_by_ptw", s.ptw_tlb_fills);
        write_vector(p + "tlb_fill_source",
            {"lamp", "ptw", "total"},
            {s.lamp_tlb_fills, s.ptw_tlb_fills, s.triggers});

        // B. Hit Where LAMP
        write_vector(p + "lamp_hit_where",
            {"l1", "l2", "llc", "miss"},
            {s.lamp_l1_hit, s.lamp_l2_hit, s.lamp_llc_hit, s.lamp_miss});

        // C. Walks Canceled by LAMP
        write_scalar(p + "walks_canceled_by_lamp", s.ptw_cancel_count);
        write_scalar(p + "ptw_requests_avoided", s.ptw_requests_avoided);
        write_vector(p + "ptw_cancel_stage",
            {"PML4", "PDP", "PD", "Leaf"},
            {s.ptw_cancel_pml4, s.ptw_cancel_pdp, s.ptw_cancel_pd, s.ptw_cancel_leaf});

        // D. TLB Miss Service Latencies (works whether STLB is ON or OFF)
        write_double(p + "avg_tlb_miss_service_latency", avg_tlb_miss_service_lat);
        write_double(p + "avg_walk_service_latency", avg_walk_lat);
        write_double(p + "avg_lamp_lookup_latency", s.hist_lamp_lookup_latency.avg());
        write_double(p + "avg_ptw_walk_latency", s.hist_ptw_translation_latency.avg());
    };

    write_side_csv("ITLB", true, cs.itlb);
    write_side_csv("DTLB", false, cs.dtlb);

    // Core-wide Combined Totals (ITLB + DTLB)
    uint64_t total_l1_tlb_misses = get_l1_tlb_misses(cpu, true, cs.itlb.load_translation_count)
                                 + get_l1_tlb_misses(cpu, false, cs.dtlb.load_translation_count);
    uint64_t total_walks = cs.itlb.load_translation_count + cs.dtlb.load_translation_count;
    uint64_t total_walk_cycles = cs.itlb.load_translation_latency_sum + cs.dtlb.load_translation_latency_sum;
    double core_avg_walk_lat = total_walks ? (double)total_walk_cycles / total_walks : 0.0;
    double core_avg_tlb_miss_service_lat = knob_stlb
        ? (total_l1_tlb_misses ? (double)(total_l1_tlb_misses * STLB_LATENCY + total_walk_cycles) / total_l1_tlb_misses : 0.0)
        : core_avg_walk_lat;

    write_scalar(core_prefix + "tlb_filled_by_lamp", cs.itlb.lamp_tlb_fills + cs.dtlb.lamp_tlb_fills);
    write_scalar(core_prefix + "tlb_filled_by_ptw", cs.itlb.ptw_tlb_fills + cs.dtlb.ptw_tlb_fills);
    write_vector(core_prefix + "lamp_hit_where",
        {"l1", "l2", "llc", "miss"},
        {cs.itlb.lamp_l1_hit + cs.dtlb.lamp_l1_hit,
         cs.itlb.lamp_l2_hit + cs.dtlb.lamp_l2_hit,
         cs.itlb.lamp_llc_hit + cs.dtlb.lamp_llc_hit,
         cs.itlb.lamp_miss + cs.dtlb.lamp_miss});
    write_scalar(core_prefix + "walks_canceled_by_lamp", cs.itlb.ptw_cancel_count + cs.dtlb.ptw_cancel_count);
    write_scalar(core_prefix + "ptw_requests_avoided", cs.itlb.ptw_requests_avoided + cs.dtlb.ptw_requests_avoided);
    write_double(core_prefix + "avg_tlb_miss_service_latency", core_avg_tlb_miss_service_lat);
    write_double(core_prefix + "avg_walk_service_latency", core_avg_walk_lat);

    // E. Cache Pollution by Leaf PTEs
    write_scalar(core_prefix + "leaf_pte_data_evictions", cs.lamp_data_evictions);
    write_scalar(core_prefix + "leaf_pte_evictions", cs.lamp_pte_evictions);
    write_scalar(core_prefix + "leaf_pte_pollution_misses", cs.lamp_pollution_misses);

    // F. ROB & Pipeline Stall Metrics
    write_scalar(core_prefix + "rob_head_translation_wait_cycles", cs.rob_head_translation_wait_cycles);
    write_scalar(core_prefix + "rob_head_data_wait_cycles", cs.rob_head_data_wait_cycles);
    write_scalar(core_prefix + "rob_head_translation_and_data_wait_cycles", cs.rob_head_translation_and_data_wait_cycles);
    write_scalar(core_prefix + "frontend_starvation_cycles", cs.frontend_starvation_cycles);
    write_scalar(core_prefix + "ifetch_translation_stall_cycles", cs.ifetch_translation_stall_cycles);
    write_scalar(core_prefix + "ifetch_data_stall_cycles", cs.ifetch_data_stall_cycles);
}
