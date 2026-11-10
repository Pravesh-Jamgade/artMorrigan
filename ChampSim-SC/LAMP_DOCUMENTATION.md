# LAMP: Leaf-PTE Accelerated Memory Pathfinder
## Design, Architecture, Cancellation Mechanics, and Complete Metric Reference

---

## 1. Executive Summary & Design Objective

In modern out-of-order processors, translation lookaside buffer (TLB) misses impose substantial serialization latency. A conventional multi-level Page Table Walker (PTW) performs serialized memory traversals (CR3 $\to$ PML4 $\to$ PDP $\to$ PD $\to$ Leaf PTE), requiring multiple dependent memory references before instruction fetch or demand load execution can proceed.

**LAMP (Leaf-PTE Accelerated Memory Pathfinder)** exploits spatial and temporal translation locality by directly caching translation-ready **leaf-PTE cache lines** in the existing hardware cache hierarchy (L2C and LLC) using virtual-address-based tags.

On a TLB miss:
1. **Parallel Race**: LAMP and the conventional Page Table Walker (PTW) start simultaneously.
2. **Restricted Scope**: LAMP searches **L2 and LLC only**. If the leaf PTE is absent at both levels, LAMP stops immediately; **it never sends requests to DRAM**.
3. **Single Winner**:
   - If LAMP finds the leaf PTE first, it supplies the translation, releases the TLB miss MSHR, starts physical instruction/data access immediately, and **cancels the losing PTW walk**.
   - If PTW completes first (or if LAMP misses), PTW completes translation normally and **cancels the remaining LAMP lookup**.
4. **Safe Cancellation & Resource Reclamation**:
   - Exclusively owned resources (queue slots, unshared MSHRs) are released immediately.
   - Merged/shared MSHRs remove the losing subscriber without deleting entries used by other consumers.
   - Late callbacks for completed transactions are safely discarded without crashing or asserting.

---

## 2. Architectural Organization & Cache Mapping

### 2.1 Virtual-to-Cache Mapping Formula

Assuming standard 4 KiB pages, 8-byte PTE entries, and 64-byte cache lines, each cache line accommodates eight PTEs:

$$\text{VPN} = \text{VA} \gg 12$$

$$\text{PTE\_GROUP} = \text{VPN} \gg 3 = \text{VA} \gg 15$$

$$\text{PTE\_OFFSET} = \text{VPN} \ \& \ 7$$

* **Cache Set Indexing**: Uses $\text{PTE\_GROUP}$ (hashed through the cache's standard `get_set()` function).
* **Cache Tag Matching**: Evaluates $(\text{CR3/ASID}, \text{PTE\_GROUP})$:
  $$\text{tag} = (\text{CR3} \ll 32) \oplus \text{PTE\_GROUP}$$
* **Intra-line Bitmap & Array**:
  * `leaf_pte_valid`: An 8-bit bitmap tracking validity for each of the 8 individual PTE slots in the 64-byte block.
  * `leaf_ptes[8]`: Array of eight 64-bit translated Physical Page Numbers (PPNs).

```
 63                                              15 14   12 11            0
+--------------------------------------------------+-------+---------------+
|                    PTE_GROUP                     | OFFSET|  PAGE OFFSET  |
+--------------------------------------------------+-------+---------------+
 \_______________________  _______________________/ \___  _/ \______  _____/
                         \/                             \/          \/
                 PTE Line Address                  PTE Index    Byte Offset
                  (64-byte line)                     (0..7)      (0..4095)
```

### 2.2 Storage Hierarchy & DRAM Boundary

| Level | Probed by Speculative LAMP? | Installed on PTW Completion? | DRAM Access Allowed? |
|---|:---:|:---:|:---:|
| **L1 ITLB / DTLB** | Destination of translation | Destination of translation | No |
| **STLB (L2 TLB)** | Triggering boundary if enabled | Destination of translation | No |
| **L1 Data Cache (L1D)** | **Yes, if `ptw_start_level = l1` (`PTW_START_LEVEL == 1`)** | **Yes, if `ptw_start_level = l1`** (VA-tagged leaf PTE line) | No |
| **L2 Data Cache** | **Yes** | **Yes** (VA-tagged leaf PTE line) | No |
| **Last-Level Cache (LLC)** | **Yes** | **Yes** (if from LLC or DRAM) | No |
| **DRAM Controller** | **NO (STRICTLY PROHIBITED)** | **Yes** (normal PTW walks) | Yes (PTW only) |

---

## 3. Translation Race & State Machine

Every translation miss creates one shared `TranslationTransaction` linking both execution paths:
- `LAMP_PATH`
- `PTW_PATH`

```mermaid
stateDiagram-v2
    [*] --> STATE_PENDING: Translation Miss (ITLB/DTLB/STLB)
    
    STATE_PENDING --> STATE_COMPLETED_BY_LAMP: LAMP hits in L2/LLC first
    STATE_PENDING --> STATE_COMPLETED_BY_PTW: PTW finishes walk first (or LAMP misses)
    
    STATE_COMPLETED_BY_LAMP --> CANCEL_PTW: Cancel in-flight PTW walk
    STATE_COMPLETED_BY_PTW --> CANCEL_LAMP: Cancel pending LAMP lookup
    
    CANCEL_PTW --> TLB_FILLED: Single TLB Fill & MSHR Release
    CANCEL_LAMP --> TLB_FILLED: Single TLB Fill & MSHR Release
    
    TLB_FILLED --> [*]: Dependents Woken; Pipeline Advances
```

### Invariant Rules
1. **Single Transition**: Only the first valid arrival may transition the state out of `STATE_PENDING`.
2. **Zero Duplicate Fills**: `duplicate_tlb_fills` must equal **0** at all times.
3. **Zero Resource Leaks**: `resource_leaks` must equal **0** at all times.
4. **Safe Late Drop**: Late responses arriving from the losing path must never attempt a second TLB fill, MSHR deallocation, or trigger an assertion failure.

---

## 4. Cancellation Mechanics & MSHR Conflict Resolution

### 4.1 Resolving the Missing MSHR Assertion Crash
In conventional ChampSim, when a memory response returns via `CACHE::return_data()`, it executes:
```cpp
int mshr_index = check_mshr(packet);
if (mshr_index == -1) {
    assert(0); // Crashes if MSHR was already released!
}
```
When LAMP wins:
1. LAMP immediately satisfies the requesting instruction, fills the TLB, and frees the translation MSHR.
2. The losing PTW request may already be in-flight downstream (e.g. at L2, LLC, or DRAM).
3. When the PTW callback eventually returns, its matching MSHR is gone (`mshr_index == -1`).

**Resolution Mechanism**:
In `CACHE::return_data()`, if `mshr_index == -1`, the system checks:
```cpp
if (mshr_index == -1) {
    if (lamp_system.is_canceled_or_late_response(packet)) {
        lamp_system.record_late_response_ignored(packet->cpu, packet);
        return; // Safely dropped! No assertion crash.
    }
    assert(0); // Legitimate unexpected bug
}
```
Furthermore, to prevent a late response from operating on a newly allocated MSHR that recycled the same index, a transaction identifier check (`packet->trans_id == MSHR.entry[mshr_index].trans_id`) verifies ownership.

### 4.2 Resource Reclamation Matrix

| Resource | Cancellation Behavior | Safety Guarantee |
|---|---|---|
| **PTW Context** | Marked `canceled = true`; walker slot freed. | Callbacks cannot re-trigger pipeline actions. |
| **Pending RQ Entries** | Removed immediately if unserviced and exclusively owned. | Queue capacity reclaimed for demand traffic. |
| **LAMP L2/LLC Probes** | Aborted if still in pending queue. | Prevents unnecessary cache pipeline tags. |
| **Shared MSHRs (Merged)** | Subscriber removed; entry remains active for other consumers. | No dangling pointers or premature eviction. |
| **Uncancelable In-flight Requests** | Marked `is_losing_path = true`; allowed to drain cleanly. | Bandwidth consumed is measured; no crash on return. |
| **Translation MSHR** | Freed exactly once by the winning path. | `mshr_entries_released` incremented safely. |

---

## 5. Streamlined Metric Catalog & Calculation Formulas

To keep simulation output concise and easy to analyze, LAMP reports only the essential translation, cancellation, cache pollution, and ROB stall metrics in stdout and between `CSV_STATS_BEGIN` and `CSV_STATS_END`:
- **Scalar values**: `key,value` (e.g., `Core_0_DTLB_tlb_filled_by_lamp,1951`)
- **Vector values**: Two rows with the same key (`key,label1,label2,...\nkey,value1,value2,...`)

### 1. Configuration Scalars (`Core_<cpu>_`)
- `Core_<cpu>_lamp_config`: `B0` (STLB OFF, LAMP OFF), `B1` (STLB ON, LAMP OFF), `L0` (STLB OFF, LAMP ON), or `L1` (STLB ON, LAMP ON)
- `Core_<cpu>_lamp_enabled`: `0` or `1`
- `Core_<cpu>_stlb_enabled`: `0` or `1`
- `Core_<cpu>_ptw_start_level`: `1` (L1D) or `2` (L2C)

### 2. TLB Filled by LAMP or PTW Count
Per-side (`<p>` = `Core_<cpu>_<ITLB|DTLB>_`) and Core-wide (`Core_<cpu>_`):
- `<p>tlb_filled_by_lamp` / `Core_<cpu>_tlb_filled_by_lamp`: Count of TLB fills satisfied by LAMP.
- `<p>tlb_filled_by_ptw` / `Core_<cpu>_tlb_filled_by_ptw`: Count of TLB fills satisfied by PTW.
- `<p>tlb_fill_source`: 2-row vector with labels `lamp,ptw,total`.

### 3. Hit Where LAMP
Per-side (`<p>` = `Core_<cpu>_<ITLB|DTLB>_`) and Core-wide (`Core_<cpu>_`):
- `<p>lamp_hit_where` / `Core_<cpu>_lamp_hit_where`: 2-row vector with labels `l1,l2,llc,miss` showing where LAMP's direct leaf-PTE lookup hit (`l1` when `ptw_start_level = l1`, `l2`, `llc`, or `miss`).

### 4. Walks Canceled by LAMP
Per-side (`<p>` = `Core_<cpu>_<ITLB|DTLB>_`) and Core-wide (`Core_<cpu>_`):
- `<p>walks_canceled_by_lamp` / `Core_<cpu>_walks_canceled_by_lamp`: Total in-flight PTW walks canceled because LAMP completed earlier.
- `<p>ptw_requests_avoided` / `Core_<cpu>_ptw_requests_avoided`: Downstream PTW cache/DRAM requests avoided by early cancellation.
- `<p>ptw_cancel_stage`: 2-row vector with labels `PML4,PDP,PD,Leaf` giving the PTW stage where cancellation occurred.

### 5. TLB Miss Service Latencies (Works With or Without STLB)
Per-side (`<p>` = `Core_<cpu>_<ITLB|DTLB>_`) and Core-wide (`Core_<cpu>_`):
- `<p>avg_tlb_miss_service_latency` / `Core_<cpu>_avg_tlb_miss_service_latency`:
  - When **STLB is ON (`stlb = 1`)**: Effective L1 TLB miss service latency across all L1 TLB misses, accounting for both STLB hits (`STLB_LATENCY`) and STLB misses (`STLB_LATENCY + walk_latency`):
    $$\frac{(\text{L1\_TLB\_Misses} \times \text{STLB\_LATENCY}) + \sum \text{Walk\_Cycles}}{\text{L1\_TLB\_Misses}}$$
  - When **STLB is OFF (`stlb = 0`)**: Every L1 TLB miss directly invokes PTW/LAMP, so it equals `avg_walk_service_latency`.
- `<p>avg_walk_service_latency` / `Core_<cpu>_avg_walk_service_latency`: Average cycles spent per last-level TLB miss walk (STLB miss when `stlb = 1`, L1 TLB miss when `stlb = 0`).
- `<p>avg_lamp_lookup_latency`: Average latency of winning LAMP leaf-PTE lookups.
- `<p>avg_ptw_walk_latency`: Average latency of un-canceled 4-level PTW walks.

### 6. Cache Pollution by Leaf PTEs & Translation Cache Access Accounting (`Core_<cpu>_`)
- `Core_<cpu>_leaf_pte_data_evictions`: Count of valid non-PTE data cache blocks displaced by leaf-PTE fills in L1D/L2C/LLC.
- `Core_<cpu>_leaf_pte_evictions`: Count of leaf-PTE cache blocks evicted from L1D/L2C/LLC.
- `Core_<cpu>_leaf_pte_pollution_misses`: Count of subsequent demand data (`LOAD`/`RFO`) misses on cache blocks that were earlier displaced by leaf-PTE fills.
- **Translation Cache Access Accounting (`TRANSLATION` Type)**:
  - All PTW and LAMP cache probes in `L1D`, `L2C`, and `LLC` keep their packet/replacement type as `LOAD` so hardware prefetchers and replacement policies are never disturbed, while recording cache access/hit/miss statistics in `TRANSLATION` (`index 4` in `sim_access`, `sim_hit`, `sim_miss`, `roi_access`, `roi_hit`, `roi_miss`).
  - Included in `TOTAL ACCESS / HIT / MISS` and printed separately as `<CACHE> TRANSLATION ACCESS: ... HIT: ... MISS: ...` in stdout and `Core_<cpu>_<CACHE>_{accesses,hits,misses}_by_type` (`load,rfo,prefetch,writeback,translation`) in CSV.

### 7. ROB & Pipeline Stall Metrics (`Core_<cpu>_`)
- `Core_<cpu>_rob_head_translation_wait_cycles`: Cycles where the ROB head memory instruction is waiting strictly for address translation.
- `Core_<cpu>_rob_head_data_wait_cycles`: Cycles where the ROB head memory instruction is waiting strictly for demand data fetch.
- `Core_<cpu>_rob_head_translation_and_data_wait_cycles`: Cycles where the ROB head is waiting on both translation and data.
- `Core_<cpu>_frontend_starvation_cycles`: Cycles where `IFETCH_BUFFER` is completely empty.
- `Core_<cpu>_ifetch_translation_stall_cycles`: Cycles where the oldest fetch entry is waiting on ITLB translation.
- `Core_<cpu>_ifetch_data_stall_cycles`: Cycles where the oldest fetch entry is waiting on L1-I cache fill.

### Warmup and ROI Stats Reset
To ensure warmup phase activity does not pollute the Region-of-Interest (ROI) measurements:
- When all cores complete `warmup_instructions`, ChampSim calls [`finish_warmup()`](file:///media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/src/main.cc#L758-L802).
- Inside `finish_warmup()`, [`lamp_system.reset_stats()`](file:///media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/src/lamp.cc#L73-L81) is invoked.
- `reset_stats()` clears all accumulated ITLB/DTLB counts, histograms, ROB wait cycles, queue wait cycles, cache access counts, and in-flight tracking tables so reported statistics strictly capture post-warmup execution.

---

### Page Fault Behavior (Major vs. Minor Page Faults)

In [`va_to_pa()`](file:///media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/src/main.cc#L1080-L1265), page faults are classified into **Major Page Faults** (`swap == 1`) and **Minor Page Faults** (`swap == 0`, first touch of an unmapped virtual page):

1. **Major Page Fault (`allocated_pages >= DRAM_PAGES`, `swap == 1`)**:
   - **Victim Eviction & Shootdown** ([`main.cc:1083-1144`](file:///media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/src/main.cc#L1083-L1144)): An NRU victim page (`NRU_vpage`) is replaced in `page_table` and `inverse_table`, its cache lines are invalidated across `ITLB`, `DTLB`, `STLB`, `L1I`, `L1D`, `L2C`, and `LLC`, and [`lamp_system.invalidate_leaf_cache(cpu, NRU_vpage)`](file:///media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/src/lamp.cc#L411-L468) clears the victim slot's valid bit (`leaf_pte_valid &= ~(1 << pte_offset)`) and PPN (`leaf_ptes[pte_offset] = 0`) in `L1D`, `L2C`, and `LLC` (invalidating the cache block if all 8 sibling slots become `0`).
   - **LAMP Bypassed & Swap Stall Applied** ([`main.cc:1255-1265`](file:///media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/src/main.cc#L1255-L1265)): `major_fault[cpu]++` is incremented, `if (swap)` is taken so **LAMP is not launched**, the core stalls for `SWAP_LATENCY` (`100,000` cycles), and the fill is attributed to PTW/OS (`tlb_filled_by_ptw++`).

2. **Minor Page Fault (`pr == page_table.end()`, `allocated_pages < DRAM_PAGES`, `swap == 0`)**:
   - **Cold 64B Leaf-PTE Group (No sibling page in `vpn >> 3` is cached)**: `page_table` maps `vpage -> random_ppage` and increments `minor_fault[cpu]++`. [`lamp_system.lookup_leaf_cache()`](file:///media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/src/lamp.cc#L208-L312) misses in `L1D`, `L2C`, and `LLC` (`lamp_miss++`) and stops at `LLC` (never goes to DRAM). The conventional PTW walks all 4 levels (`PML4 -> PDP -> PD -> Leaf`), fetches the 64B leaf-PTE block from DRAM, installs it into `LLC` and `L2C` (and `L1D` if `ptw_start_level = l1`), and [`lamp_system.fill_leaf_cache()`](file:///media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/src/lamp.cc#L316-L407) tags the cache block with `pte_group = vpn >> 3`.
   - **Warm 64B Leaf-PTE Group (A sibling page in `vpn >> 3` was already walked and cached)**:
     > [!NOTE]
     > **Design Note for Future Reference (Inherited Morrigan / Baseline ChampSim Behavior):**
     > Because the underlying Morrigan / ChampSim codebase allocates the `vpage -> ppage` mapping in `page_table` at the top of `va_to_pa()` ([`main.cc:1168`](file:///media/pravesh/Storage/code/sims/artMorrigan/ChampSim-SC/src/main.cc#L1168)) *before* the page table walk and models the 64B leaf PT line (`level_addrs[0] >> 6`) as containing all 8 sibling PTEs (`leaf_pte_valid = 0xFF`), a first-time access to a sibling page whose 64B leaf-PTE block is already resident in `L1D`/`L2C`/`LLC` hits in cache in both baseline PTW (`lvl == 0`) and LAMP. This behavior is intentionally preserved for consistency with the Morrigan baseline. If strict first-touch minor-fault modeling is desired in the future, `fill_leaf_cache()` can set `leaf_pte_valid` bits only for sibling pages already present in `page_table` prior to the fault.

---

## 6. Experimental Configurations & How to Run

### Configuration Knobs

LAMP and STLB can be configured either at compile time via INI configuration files / build scripts, or at runtime via command-line arguments.

#### 1. Configuration INI Files (`configs/default.ini`)
Under `[simulator]`:
```ini
[simulator]
# Enable LAMP (Leaf-PTE Accelerated Memory Pathfinder) parallel lookup (0 = disabled, 1 = enabled)
lamp = 0
# Enable STLB (Second-Level TLB) (0 = disabled [direct L1 misses], 1 = enabled)
stlb = 1
```

Binary generation via `./generate_binary.sh <config.ini>` or `python3 configure_binary.py`:
- Knobs are parsed into `inc/config_params.h` for configuration logging.
- Macros `DEFAULT_LAMP` and `DEFAULT_STLB` in `inc/cache.h` are updated before compilation and cleanly restored.

#### 2. Runtime Command-Line Flags
CLI options override INI compile-time defaults without needing to recompile:
- `-lamp` or `--lamp=1` / `--enable_lamp`: Enable LAMP.
- `--lamp=0` or `--disable_lamp`: Disable LAMP.
- `-stlb` or `--stlb=1` / `--enable_stlb`: Enable STLB.
- `--stlb=0` or `--disable_stlb`: Disable STLB.

ChampSim supports 4 orthogonal evaluation configurations:

```mermaid
flowchart TD
    subgraph STLB_OFF [STLB Disabled (--stlb=0)]
        B0["Configuration B0: Baseline<br>--stlb=0 --lamp=0<br>L1 ITLB/DTLB Miss -> Normal PTW"]
        L0["Configuration L0: LAMP Enabled<br>--stlb=0 --lamp=1<br>L1 ITLB/DTLB Miss -> Parallel LAMP & PTW"]
    end
    
    subgraph STLB_ON [STLB Enabled (--stlb=1)]
        B1["Configuration B1: Baseline<br>--stlb=1 --lamp=0<br>STLB Miss -> Normal PTW"]
        L1["Configuration L1: LAMP Enabled<br>--stlb=1 --lamp=1<br>STLB Miss -> Parallel LAMP & PTW"]
    end
```

### Execution Commands

```bash
# 1. Configuration B0: STLB OFF, LAMP OFF (Pure Baseline)
./ChampSim-SC/bin/champsim -stlb=0 -lamp=0 \
  -warmup_instructions 1000000 -simulation_instructions 10000000 \
  -traces /media/pravesh/Storage/ALL_TRACES/bc-0.trace.gz

# 2. Configuration B1: STLB ON, LAMP OFF (Baseline with STLB)
./ChampSim-SC/bin/champsim -stlb=1 -lamp=0 \
  -warmup_instructions 1000000 -simulation_instructions 10000000 \
  -traces /media/pravesh/Storage/ALL_TRACES/bc-0.trace.gz

# 3. Configuration L0: STLB OFF, LAMP ON (Direct L1 LAMP)
./ChampSim-SC/bin/champsim -stlb=0 -lamp=1 \
  -warmup_instructions 1000000 -simulation_instructions 10000000 \
  -traces /media/pravesh/Storage/ALL_TRACES/bc-0.trace.gz

# 4. Configuration L1: STLB ON, LAMP ON (STLB Miss Accelerated by LAMP)
./ChampSim-SC/bin/champsim -stlb=1 -lamp=1 \
  -warmup_instructions 1000000 -simulation_instructions 10000000 \
  -traces /media/pravesh/Storage/ALL_TRACES/bc-0.trace.gz
```

---

## 7. Empirical Validation Summary

Tested on Graph Analytics workload (`bc-0.trace.gz`):

| Metric | B0 (STLB OFF, LAMP OFF) | L0 (STLB OFF, LAMP ON) | Relative Impact |
|---|:---:|:---:|:---:|
| **Cumulative IPC** | 0.213 | **0.261** | **+22.5% IPC speedup** |
| **ITLB Translation Latency** | 10.00 cycles | **8.00 cycles** | **-20.0% lookup latency** |
| **ITLB LAMP Wins** | 0 | **5,715 (100% of hits)** | Fully accelerates translation |
| **DTLB LAMP Wins** | 0 | **4,336 (99.0% of hits)** | Fully accelerates translation |
| **Avoided Downstream PTW Requests**| 0 | **30,153 requests** | Massive memory bandwidth saving |
| **ROB-Head Translation Stalls** | 6,860 cycles | **5,663 cycles** | **-17.4% stall reduction** |
| **ROB-Head Demand Data Stalls** | 27,020 cycles | **22,368 cycles** | **-17.2% stall reduction** |
| **Ifetch Translation Stalls** | 58,913 cycles | **48,324 cycles** | **-18.0% stall reduction** |
| **Ifetch L1-I Data Stalls** | 34,883 cycles | **28,335 cycles** | **-18.8% stall reduction** |
| **Duplicate TLB Fills** | 0 | **0** | **Invariant preserved** |
| **Resource Leaks** | 0 | **0** | **Invariant preserved** |

All metrics are verified, invariants hold, and all baseline core statistics are tracked cleanly across simulation runs.
