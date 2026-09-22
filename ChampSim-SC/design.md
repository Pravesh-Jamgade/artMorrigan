# STLB Block Architecture and Design Specification

This document provides a detailed technical specification of the Second-Level Translation Lookaside Buffer (STLB) block modes implemented in ChampSim-SC: **STLB Block Detail Mode** and **STLB Block Sparsity Mode**.

---

## 1. Overview & Motivation

Traditional STLB entries cache individual 4KB Page Table Entries (PTEs). To improve STLB reach and reduction of Page Table Walks (PTWs), STLB block architectures aggregate multiple PTEs into a single multi-sector block entry (`STLB_PTES_PER_BLOCK = 4` sectors).

ChampSim-SC supports two distinct operational modes for STLB block organization, configured via `stlb_mode` in configuration files:
1. **Detail Mode** (`STLB_BLOCK_DETAIL` / `"detail"`): Caches consecutive, contiguous neighbouring PTEs.
2. **Sparsity Mode** (`STLB_BLOCK_SPARSITY` / `"sparsity"`): Uses dynamic footprint learning from upper cache levels (L2C / LLC) to select non-contiguous, high-re-reference valid PTEs within an 8-PTE (4KB Page Table block) window.

---

## 2. Hardware Data Structures (`STLB_BLOCK_ENTRY`)

Each STLB set contains `NUM_WAY` entries represented by `STLB_BLOCK_ENTRY` (`inc/cache.h`):

```cpp
struct STLB_BLOCK_ENTRY {
    uint64_t tag;                                    // Block Virtual Page Number (VPN)
    uint64_t pte[STLB_PTES_PER_BLOCK];               // Array of PPN physical translations (4 slots)
    uint8_t entry_offset_in_block[STLB_PTES_PER_BLOCK]; // 3-bit sub-page offset (0..7) for each sector slot (Sparsity Mode)
    uint32_t lru;                                    // LRU replacement state
    uint8_t valid_mask;                              // Bitmask indicating valid sector slots (1 bit per slot)
    uint8_t accessed_mask;                           // Bitmask tracking re-reference / usage during residency
};
```

---

## 3. STLB Block Detail Mode (`stlb_mode = detail`)

### 3.1 VPN Decomposition & Tag Indexing
- **Block Granularity**: `STLB_PTES_PER_BLOCK = 4` consecutive PTEs.
- **`block_vpn`**: `vpn / STLB_PTES_PER_BLOCK`
- **Sub-block Offset**: `vpn % STLB_PTES_PER_BLOCK` (values `0..3`).

### 3.2 Lookup Logic (`stlb_block_lookup`)
1. Compute `block_vpn = vpn / 4` and set index `set = get_set(block_vpn)`.
2. Iterate through ways `0..NUM_WAY-1`:
   - If `entry.valid_mask` is non-zero and `entry.tag == block_vpn`:
     - Check if sector slot `offset = vpn % 4` is valid (`(entry.valid_mask & (1 << offset)) != 0`).
     - **Hit**: Mark `entry.accessed_mask |= (1 << offset)`, update block LRU, and return translated PPN `entry.pte[offset]`.
     - **Miss**: Block tag matched, but the specific sub-PTE sector is invalid.

### 3.3 Fill Logic (`stlb_block_fill`)
1. **Way Selection**:
   - Check if a block with matching `block_vpn` exists in the set.
   - If missing, select an invalid way (`!valid_mask`).
   - If all ways are valid, select the LRU way (`lru == NUM_WAY - 1`) and evict it (`evict_stlb_block`).
2. **Population**:
   - Set `entry.tag = block_vpn`.
   - Iterate through offsets `0..3`:
     - Query page table for `block_vpn * 4 + offset`. If allocated, populate `entry.pte[offset]` and set `entry.valid_mask |= (1 << offset)`.
   - Set `entry.accessed_mask |= (1 << (vpn % 4))`.
   - Promote way to LRU position `0`.

---

## 4. STLB Block Sparsity Mode (`stlb_mode = sparsity`)

### 4.1 Concept & Motivation
In workloads with non-consecutive page access patterns (e.g., strided or sparse memory access), fetching 4 consecutive PTEs wastes sector slots on unused translations. **Sparsity Mode** operates at an **8-PTE Page Table line window** (`block_vpn = vpn / 8`), storing 4 dynamically selected sub-page offsets (`0..7`) in sector slots (`0..3`).

### 4.2 Hardware Indexing & Tag Decomposition
- **Block Granularity**: 8 PTEs per 4KB leaf PT line.
- **`block_vpn`**: `vpn / 8`
- **Requested Sub-page Offset**: `req_offset = vpn % 8` (values `0..7`).
- **Offset Field**: `entry_offset_in_block[s]` stores the 3-bit sub-page offset assigned to sector slot `s`.

---

### 4.3 Detailed Operational Flow

```mermaid
flowchart TD
    A[STLB Fill Request for VPN] --> B[Calculate block_vpn = vpn / 8, req_offset = vpn % 8]
    B --> C{Matching block_vpn in STLB set?}
    
    C -- Yes (Block Hit, Missing PTE) --> D[CASE A: In-Place Slot Update]
    D --> D1[Lookup requested PPN for req_offset]
    D1 --> D2{Un-allocated slot available?}
    D2 -- Yes --> D3[Install in invalid slot]
    D2 -- No --> D4{Un-accessed slot available?}
    D4 -- Yes --> D5[Replace un-accessed slot]
    D4 -- No --> D6[Fallback: Replace slot 0]
    D3 --> D7[Set valid_mask & accessed_mask for target slot]
    D5 --> D7
    D6 --> D7
    D7 --> D8[Update LRU to 0 & Return]
    
    C -- No (Block Miss) --> E[CASE B: New Block Allocation]
    E --> E1{Invalid Way in Set?}
    E1 -- Yes --> E2[Select Invalid Way]
    E1 -- No --> E3[Evict LRU Way (lru == NUM_WAY - 1)]
    E2 --> F[Check hit_where Information]
    E3 --> F
    
    F --> F1{hit_where == 3 (LLC) or 4 (DRAM)?}
    F1 -- Yes --> F2[Query LLC translation_footprint first, fallback to L2C]
    F1 -- No --> F3[Query L2C translation_footprint first, fallback to LLC]
    
    F2 --> G[Gather Valid 8-PTE PPNs & Footprint Mask]
    F3 --> G
    
    G --> H[Slot 0: Fill requested req_offset]
    H --> I[Slots 1..3: Fill prior footprint PTEs from L2C/LLC mask]
    I --> J[Remaining Slots: Fill leftover valid PTEs in order 0..7]
    J --> K[Mark accessed_mask for req_offset slot, Update LRU to 0]
```

---

### 4.4 Lookup Logic (`stlb_block_lookup`)
1. Compute `block_vpn = vpn / 8` and `req_offset = vpn % 8`.
2. Search set `set = get_set(block_vpn)` for matching way with `tag == block_vpn` and `valid_mask != 0`.
3. If block found, search sector slots `s` from `0..3`:
   - If slot `s` is valid (`(entry.valid_mask & (1 << s)) != 0`) and `entry.entry_offset_in_block[s] == req_offset`:
     - **Hit**: Mark `entry.accessed_mask |= (1 << s)`, update LRU position to `0`, and return PPN `entry.pte[s]`.
4. Return **Miss** if block or slot not found.

---

### 4.5 Fill & In-Place Replacement Logic (`stlb_block_fill`)

#### **Case A: Block Already Resident (`existing_way != -1`)**
When a miss occurs for a requested PTE whose parent PT block (`block_vpn`) is already cached in the STLB:
1. Do **not** evict or reset the existing STLB block.
2. Lookup the PPN for `req_offset`.
3. Select an internal target slot `target_slot`:
   - **Preference 1**: First un-allocated (invalid) slot (`!(entry.valid_mask & (1 << s))`).
   - **Preference 2**: First un-accessed slot (`!(entry.accessed_mask & (1 << s))`).
   - **Fallback**: Replace a random slot (`rand() % STLB_PTES_PER_BLOCK`) if all slots are occupied and accessed.
4. Overwrite slot `target_slot`:
   - `entry.pte[target_slot] = req_ppn`
   - `entry.entry_offset_in_block[target_slot] = req_offset`
   - `entry.valid_mask |= (1 << target_slot)`
   - `entry.accessed_mask |= (1 << target_slot)`
5. Promote block LRU position to `0`.

#### **Case B: New Block Allocation (`existing_way == -1`)**
1. **Way Replacement Selection**:
   - Prefer an invalid way (`!valid_mask`).
   - If all ways are valid, select the least-recently-used way (`lru == NUM_WAY - 1`) and call `evict_stlb_block(entry)` to record eviction footprint stats.
2. **`hit_where`-Aware Footprint Fetching**:
   - Retrieve `hit_where` from the PTW packet (0=PWC, 1=L1D, 2=L2C, 3=LLC, 4=DRAM).
   - If `hit_where == 3` (LLC hit) or `hit_where == 4` (DRAM hit / LLC fill):
     - **Check LLC first** for the 8-PTE line's `translation_footprint`, falling back to L2C if missing. *(Rationale: A newly allocated L2 line contains only 1 accessed bit, whereas the LLC line retains the full accumulated footprint history).*
   - Otherwise, **check L2C first**, then LLC.
3. **Sector Slot Priority Population**:
   - **Step 1**: Fill slot `0` with the requested `req_offset` (if valid).
   - **Step 2**: Fill remaining slots with offsets present in the L2C/LLC `footprint_mask`.
   - **Step 3**: Fill any leftover empty slots with valid allocated PTEs in natural order (`0..7`).
   - **Step 4**: Mark `accessed_mask` bit for `req_offset`'s slot and update LRU to `0`.

---

## 5. Summary Comparison Matrix

| Property / Behavior | Detail Mode (`stlb_mode = detail`) | Sparsity Mode (`stlb_mode = sparsity`) |
| :--- | :--- | :--- |
| **Block Granularity** | 4 consecutive PTEs (`vpn / 4`) | 8-PTE PT line window (`vpn / 8`) |
| **Sector Slots per Block** | 4 fixed slots (`0..3`) | 4 dynamic slots storing 3-bit offsets (`0..7`) |
| **PTE Placement** | Rigid offset mapping (`vpn % 4`) | Dynamic footprint & priority allocation |
| **Existing Block Miss Handle** | Full block overwrite | In-place slot insertion (preserves valid/accessed entries) |
| **Footprint Source** | Fixed contiguous neighborhood | `hit_where`-guided L2C/LLC footprint mask |
| **Way Eviction Policy** | Invalid way preferred, else LRU (`lru == NUM_WAY - 1`) | Invalid way preferred, else LRU (`lru == NUM_WAY - 1`) |
