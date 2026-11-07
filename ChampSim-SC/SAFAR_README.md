# SAFAR: Sector-Level Miss Repeatability and RRC × Footprint Persistence Analysis

## 1. Overview
The SAFAR characterization extension instruments ChampSim's STLB and Page Table Walker (PTW) to analyze translation sector miss recurrence and evaluate whether translation sectors (groups of adjacent PTEs) responsible for expensive leaf page-table walks (LLC/DRAM-bound) repeatedly miss, and whether their re-reference count (RRC) and footprint characteristics remain persistent across successive STLB residencies.

This is a **characterization-only** measurement. It does not modify STLB replacement, insertion, capacity, timing, or page-table-walk latencies.

---

## 2. Sector Identity & Indexing Semantics
- **Sector Definition**: Translation sectors aggregate $N = \text{STLB\_PTES\_PER\_BLOCK}$ consecutive Page Table Entries.
- **Canonical Sector Identifier**:
  $$\text{sector\_id} = \frac{\text{unique\_vpage}}{\text{STLB\_PTES\_PER\_BLOCK}}$$
  Where `unique_vpage` already incorporates address space separation (e.g. core / ASID differentiation via bit manipulation).
- **PTE Offset within Sector**:
  $$\text{offset} = \text{unique\_vpage} \pmod{\text{STLB\_PTES\_PER\_BLOCK}}$$
- **Independence**: Sector-level history and repetition are tracked completely independently of the baseline individual VPN compulsory/non-compulsory tracking. All baseline VPN statistics, counters, and matrices remain 100% unaltered.

---

## 3. Object-Oriented Architecture (`SectorTracker` & `SectorHistory`)
Implemented in [`inc/sector_analysis.h`](file:///media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/inc/sector_analysis.h) and [`src/sector_analysis.cc`](file:///media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/src/sector_analysis.cc):
- **`SectorHistory`**:
  - `total_misses`: Total STLB miss episodes experienced by this sector.
  - `noncompulsory_misses`: Number of non-compulsory miss episodes.
  - `l2_bound_misses`, `llc_bound_misses`, `dram_bound_misses`: Counts of leaf-PTE service sources.
  - `expensive_misses`: Cumulative LLC + DRAM refill events (`llc_bound_misses + dram_bound_misses`).
  - `missed_offset_bitmap`: Bitmask of constituent PTE offsets within the sector that caused misses.
  - `same_offset_repeat_misses`, `diff_offset_repeat_misses`: Sub-sector spatial vs. temporal locality metrics.
  - `last_residency`: Snapshot of the previous completed STLB residency (`ResidencySnapshot`).
- **`ResidencySnapshot`**:
  - Stores `rrc`, `footprint`, `fill_source`, and `valid` flag from the prior eviction.
- **`SectorTracker`**:
  - Encapsulates the sector history table (`std::unordered_map<uint64_t, SectorHistory>`), transition matrices, repeat-conditioned RRC × footprint matrices, and reporting functions.

---

## 4. Sampling Points & Classification Semantics
1. **At STLB Miss Event (`record_miss`)**:
   - Triggered inside `va_to_pa()` when a translation request misses the STLB.
   - Updates `sh.total_misses`, `total_sector_misses`, and `sh.noncompulsory_misses`.
   - If `sh.total_misses >= 2`, increments `total_repeated_sector_miss_events`.
   - Updates offset bitmap and tracks same-offset vs. different-offset repeat misses.
2. **At Leaf-PTE Fill Determination (`record_fill_source`)**:
   - Triggered exactly once per completed leaf PTW when the service source (`hit_where`: 2=L2, 3=LLC, 4=DRAM) is resolved.
   - Updates `l2_bound_misses`, `llc_bound_misses`, `dram_bound_misses`, and `expensive_misses`.
   - Records the predictive mapping: `(prev_rrc_bucket, prev_footprint) -> next_fill_source`.
3. **At STLB Eviction / Completed Residency (`record_completed_residency`)**:
   - Triggered inside `evict_stlb_block()` when an STLB block completes its residency.
   - Populates repeat-conditioned RRC × Footprint matrices:
     - `SECTOR_REPEAT2_RRC_FOOTPRINT`: sectors with `total_misses >= 2`.
     - `SECTOR_REPEAT3_RRC_FOOTPRINT`: sectors with `total_misses >= 3`.
     - `SECTOR_EXPENSIVE_REPEAT2_RRC_FOOTPRINT`: sectors with `expensive_misses >= 2`.
     - `SECTOR_EXPENSIVE_REPEAT3_RRC_FOOTPRINT`: sectors with `expensive_misses >= 3`.
   - Records behavioral persistence transitions:
     - `SECTOR_RRC_TRANSITIONS`: `previous_rrc_bucket x current_rrc_bucket`.
     - `SECTOR_FOOTPRINT_TRANSITIONS`: `previous_footprint x current_footprint`.
     - `SECTOR_FILL_SOURCE_TRANSITIONS`: `previous_source x current_source`.
   - Updates `last_residency` snapshot for the next cycle.

---

## 5. ROI and Warmup Policy
- During warmup resets (`reset_cache_stats()` in `src/main.cc`), `sector_tracker.reset()` is invoked for all cores.
- All history tables, episode counters, persistence transitions, and repeat matrices are cleared, ensuring that warmup misses do not pollute Region-of-Interest (ROI) measurements.

---

## 6. Output Metrics & CSV Table Formats

### Scalars (Console & CSV)
- `sector_repeat_unique_sectors`: Total unique sectors observed during ROI.
- `sector_repeat_sectors_with_ge2_misses`: Sectors with $\ge 2$ STLB misses.
- `sector_repeat_sectors_with_ge3_misses`: Sectors with $\ge 3$ STLB misses.
- `sector_repeat_total_miss_events_repeated_sectors`: Total miss events belonging to sectors with repeated misses.
- `sector_repeat_llc_events_repeated_sectors`: Total LLC-bound refill events belonging to repeated sectors.
- `sector_repeat_dram_events_repeated_sectors`: Total DRAM-bound refill events belonging to repeated sectors.
- `sector_repeat_sectors_with_ge2_expensive_refills`: Sectors with $\ge 2$ expensive (LLC+DRAM) refills.
- `sector_repeat_sectors_with_ge3_expensive_refills`: Sectors with $\ge 3$ expensive (LLC+DRAM) refills.
- `sector_repeat_same_offset_repeat_misses`: Repeat misses targeting an already-observed offset in the sector.
- `sector_repeat_diff_offset_repeat_misses`: Repeat misses targeting a new offset within an already-observed sector.

### Tables
1. **`SECTOR_MISS_FREQUENCY_HISTOGRAM`**:
   - Bins: `1`, `2`, `3-4`, `5-8`, `9-16`, `17-32`, `33+`.
   - Columns: `episodes`, `unique_sectors`, `l2_events`, `llc_events`, `dram_events`.
2. **`SECTOR_REPEAT2_RRC_FOOTPRINT_<BOUND>`** & **`SECTOR_REPEAT3_RRC_FOOTPRINT_<BOUND>`**:
   - Evaluated for L2, LLC, and DRAM bounds.
3. **`SECTOR_EXPENSIVE_REPEAT2_RRC_FOOTPRINT_<BOUND>`** & **`SECTOR_EXPENSIVE_REPEAT3_RRC_FOOTPRINT_<BOUND>`**:
   - Evaluated for LLC and DRAM bounds.
4. **`SECTOR_RRC_TRANSITIONS`**:
   - Dimensions: $7 \times 7$ matrix (`rrc0`, `rrc1`, `rrc2`, `rrc4`, `rrc8`, `rrc16`, `rrc32`).
5. **`SECTOR_FOOTPRINT_TRANSITIONS`**:
   - Dimensions: $\text{STLB\_PTES\_PER\_BLOCK} \times \text{STLB\_PTES\_PER\_BLOCK}$.
6. **`SECTOR_FILL_SOURCE_TRANSITIONS`**:
   - Dimensions: $3 \times 3$ matrix (L2, LLC, DRAM).
7. **`SECTOR_PREV_FEATURES_TO_NEXT_SOURCE`**:
   - Maps `(prev_rrc, prev_fp)` to next refill counts in `(next_l2, next_llc, next_dram)`.
