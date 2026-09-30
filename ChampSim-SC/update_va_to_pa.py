import re

with open('src/main.cc', 'r') as f:
    lines = f.readlines()

start_line = -1
end_line = -1

for i, line in enumerate(lines):
    if "for (uint32_t level = 0; level < 3; ++level)" in line and "mmu_hit[level]" in lines[i+1]:
        start_line = i
    if "if (PAGE_TABLE_LATENCY != 0){" in line and "magic != 2" in lines[i+1]:
        end_line = i

if start_line != -1 and end_line != -1:
    new_code = """\t\tfor (uint32_t level = 0; level < 3; ++level)
\t\t\tif (mmu_hit[level])
\t\t\t\tooo_cpu[cpu].STLB.pagetable_pwc_hits[level]++;

\t\tuint64_t cr3 = 0x200000;
\t\tuint64_t ptes_per_page = PAGE_SIZE / 8;
\t\tuint64_t log2_ptes = lg2(ptes_per_page);
\t\tuint64_t pte_mask = ptes_per_page - 1;
\t\tint walk_depth = (48 - LOG2_PAGE_SIZE + log2_ptes - 1) / log2_ptes;

\t\tuint64_t level_addrs[10];
\t\tfor (int i = 0; i < walk_depth; i++) {
\t\t\tuint64_t offset = 0;
\t\t\tfor (int j = i + 1; j < walk_depth; j++) {
\t\t\t\tuint64_t multiplier = 1;
\t\t\t\tfor (int k = 0; k < walk_depth - j; k++) multiplier *= ptes_per_page;
\t\t\t\toffset += multiplier * 8;
\t\t\t}
\t\t\tuint64_t index_offset = 0;
\t\t\tfor (int j = i; j < walk_depth; j++) {
\t\t\t\tuint64_t idx = (vpage >> (j * log2_ptes)) & pte_mask;
\t\t\t\tuint64_t multiplier = 1;
\t\t\t\tfor (int k = 0; k < j - i; k++) multiplier *= ptes_per_page;
\t\t\t\tindex_offset += idx * multiplier * 8;
\t\t\t}
\t\t\tlevel_addrs[i] = cr3 + offset + index_offset;
\t\t}

\t\tcstall = 2;
\t\tuint32_t set, way_fill;
\t\tint way_read;
\t\tint debug = 0;
\t\tif (debug) cout << "\\nNEW PAGE WALK " << endl;

\t\tint asap = ASAP;
\t\tint ideal = iflag * IDEAL;
\t\tPACKET search_packet;

\t\tif (!asap && ideal != 1) {
\t\t\tfor (int lvl = walk_depth - 1; lvl >= 0; --lvl) {
\t\t\t\t// For standard 4-level: lvl=3 (PML4), lvl=2 (PDP), lvl=1 (PD), lvl=0 (PT)
\t\t\t\t// MMU hit index: 0 for PML4, 1 for PDP, 2 for PD
\t\t\t\tint mapped_mmu_idx = (walk_depth > 3 && lvl >= 1 && lvl <= 3) ? (3 - lvl) : -1;
\t\t\t\tif (walk_depth == 3 && lvl >= 1 && lvl <= 2) mapped_mmu_idx = 2 - lvl;
\t\t\t\t
\t\t\t\tbool hit = false;
\t\t\t\tif (mapped_mmu_idx >= 0 && mapped_mmu_idx < 3) {
\t\t\t\t\thit = mmu_hit[mapped_mmu_idx];
\t\t\t\t}
\t\t\t\tif (walk_depth > 4 && lvl >= 4) {
\t\t\t\t\thit = false; // No MMU cache for these levels
\t\t\t\t}

\t\t\t\t// stat_idx is 0 for PML4/root, 1 for PDP, 2 for PD, 3 for PT
\t\t\t\tint stat_idx = 3 - lvl;
\t\t\t\tif (stat_idx < 0) stat_idx = 0;
\t\t\t\tif (stat_idx > 3) stat_idx = 3;

\t\t\t\tif (!hit) {
\t\t\t\t\tuint64_t addr = level_addrs[lvl];
\t\t\t\t\tsearch_packet.address = addr >> LOG2_BLOCK_SIZE;
\t\t\t\t\tsearch_packet.full_addr = addr;

\t\t\t\t\tset = ooo_cpu[cpu].L1D.get_set(addr >> LOG2_BLOCK_SIZE);
\t\t\t\t\tway_read = PTW_START_LEVEL == 1 ? ooo_cpu[cpu].L1D.check_hit(&search_packet) : -1;

\t\t\t\t\tif (way_read >= 0) {
\t\t\t\t\t\tif (lvl == 0) search_packet.hit_where = 1;
\t\t\t\t\t\tooo_cpu[cpu].L1D.mark_translation_access(set, way_read, addr, stat_idx);
\t\t\t\t\t\tooo_cpu[cpu].STLB.pagetable_mr_hit_ratio[stat_idx][0]++;
\t\t\t\t\t\tif (!magic) {
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.update_replacement_state(cpu, set, way_read, ooo_cpu[cpu].L1D.block[set][way_read].full_addr, ip, (lvl==0)?type:0, type, 1);
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.block[set][way_read].used = 1;
\t\t\t\t\t\t}
\t\t\t\t\t\tcstall += PTW_START_LEVEL == 1 ? L1D_LATENCY : 0;
\t\t\t\t\t} else {
\t\t\t\t\t\tcstall += PTW_START_LEVEL == 1 ? L1D_LATENCY : 0;

\t\t\t\t\t\tif (!magic && PTW_START_LEVEL == 1) {
\t\t\t\t\t\t\tway_fill = ooo_cpu[cpu].L1D.find_victim(cpu, instr_id, set, ooo_cpu[cpu].L1D.block[set], ip, addr, type);
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.record_footprint_on_eviction(set, way_fill);
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.update_replacement_state(cpu, set, way_fill, addr, ip, (lvl==0)?ooo_cpu[cpu].L1D.block[set][way_fill].full_addr:0, type, 0);

\t\t\t\t\t\t\tif (ooo_cpu[cpu].L1D.block[set][way_fill].valid == 0) ooo_cpu[cpu].L1D.block[set][way_fill].valid = 1;
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.block[set][way_fill].dirty = 0;
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.block[set][way_fill].prefetch = 0;
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.block[set][way_fill].used = 0;
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.block[set][way_fill].tag = addr >> LOG2_BLOCK_SIZE;
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.block[set][way_fill].address = addr >> LOG2_BLOCK_SIZE;
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.block[set][way_fill].full_addr = addr;
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.mark_translation_access(set, way_fill, addr, stat_idx, false);
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.block[set][way_fill].data = 55;
\t\t\t\t\t\t\tooo_cpu[cpu].L1D.block[set][way_fill].cpu = cpu;
\t\t\t\t\t\t}

\t\t\t\t\t\tset = ooo_cpu[cpu].L2C.get_set(addr >> LOG2_BLOCK_SIZE);
\t\t\t\t\t\tway_read = ooo_cpu[cpu].L2C.check_hit(&search_packet);

\t\t\t\t\t\tif (way_read >= 0) {
\t\t\t\t\t\t\tif (lvl == 0) search_packet.hit_where = 2;
\t\t\t\t\t\t\tooo_cpu[cpu].L2C.mark_translation_access(set, way_read, addr, stat_idx);
\t\t\t\t\t\t\tooo_cpu[cpu].STLB.pagetable_mr_hit_ratio[stat_idx][1]++;
\t\t\t\t\t\t\tif (!magic) {
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.update_replacement_state(cpu, set, way_read, ooo_cpu[cpu].L2C.block[set][way_read].full_addr, ip, (lvl==0)?type:0, type, 1);
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.block[set][way_read].used = 1;
\t\t\t\t\t\t\t}
\t\t\t\t\t\t\tcstall += L2C_LATENCY;
\t\t\t\t\t\t} else {
\t\t\t\t\t\t\tcstall += L2C_LATENCY;

\t\t\t\t\t\t\tif (!magic) {
\t\t\t\t\t\t\t\tway_fill = ooo_cpu[cpu].L2C.find_victim(cpu, instr_id, set, ooo_cpu[cpu].L2C.block[set], ip, addr, type);
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.record_footprint_on_eviction(set, way_fill);
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.update_replacement_state(cpu, set, way_fill, addr, ip, (lvl==0)?ooo_cpu[cpu].L2C.block[set][way_fill].full_addr:0, type, 0);

\t\t\t\t\t\t\t\tif (ooo_cpu[cpu].L2C.block[set][way_fill].valid == 0) ooo_cpu[cpu].L2C.block[set][way_fill].valid = 1;
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.block[set][way_fill].dirty = 0;
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.block[set][way_fill].prefetch = 0;
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.block[set][way_fill].used = 0;
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.block[set][way_fill].tag = addr >> LOG2_BLOCK_SIZE;
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.block[set][way_fill].address = addr >> LOG2_BLOCK_SIZE;
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.block[set][way_fill].full_addr = addr;
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.mark_translation_access(set, way_fill, addr, stat_idx, false);
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.block[set][way_fill].data = 55;
\t\t\t\t\t\t\t\tooo_cpu[cpu].L2C.block[set][way_fill].cpu = cpu;
\t\t\t\t\t\t\t}

\t\t\t\t\t\t\tset = uncore.LLC.get_set(addr >> LOG2_BLOCK_SIZE);
\t\t\t\t\t\t\tway_read = uncore.LLC.check_hit(&search_packet);

\t\t\t\t\t\t\tif (way_read >= 0) {
\t\t\t\t\t\t\t\tif (lvl == 0) search_packet.hit_where = 3;
\t\t\t\t\t\t\t\tuncore.LLC.mark_translation_access(set, way_read, addr, stat_idx);
\t\t\t\t\t\t\t\tooo_cpu[cpu].STLB.pagetable_mr_hit_ratio[stat_idx][2]++;
\t\t\t\t\t\t\t\tif (!magic) {
\t\t\t\t\t\t\t\t\tuncore.LLC.llc_update_replacement_state(cpu, set, way_read, uncore.LLC.block[set][way_read].full_addr, ip, 0, type, 1);
\t\t\t\t\t\t\t\t\tuncore.LLC.block[set][way_read].used = 1;
\t\t\t\t\t\t\t\t}
\t\t\t\t\t\t\t\tcstall += LLC_LATENCY;
\t\t\t\t\t\t\t} else {
\t\t\t\t\t\t\t\tif (lvl == 0) search_packet.hit_where = 4;
\t\t\t\t\t\t\t\tcstall += LLC_LATENCY;

\t\t\t\t\t\t\t\tif (!magic) {
\t\t\t\t\t\t\t\t\tway_fill = uncore.LLC.llc_find_victim(cpu, instr_id, set, uncore.LLC.block[set], ip, addr, type);
\t\t\t\t\t\t\t\t\tuncore.LLC.record_footprint_on_eviction(set, way_fill);
\t\t\t\t\t\t\t\t\tuncore.LLC.llc_update_replacement_state(cpu, set, way_fill, addr, ip, (lvl==0)?uncore.LLC.block[set][way_fill].full_addr:0, type, 0);

\t\t\t\t\t\t\t\t\tif (uncore.LLC.block[set][way_fill].valid == 0) uncore.LLC.block[set][way_fill].valid = 1;
\t\t\t\t\t\t\t\t\tuncore.LLC.block[set][way_fill].dirty = 0;
\t\t\t\t\t\t\t\t\tuncore.LLC.block[set][way_fill].prefetch = 0;
\t\t\t\t\t\t\t\t\tuncore.LLC.block[set][way_fill].used = 0;
\t\t\t\t\t\t\t\t\tuncore.LLC.block[set][way_fill].tag = addr >> LOG2_BLOCK_SIZE;
\t\t\t\t\t\t\t\t\tuncore.LLC.block[set][way_fill].address = addr >> LOG2_BLOCK_SIZE;
\t\t\t\t\t\t\t\t\tuncore.LLC.block[set][way_fill].full_addr = addr;
\t\t\t\t\t\t\t\t\tuncore.LLC.mark_translation_access(set, way_fill, addr, stat_idx, false);
\t\t\t\t\t\t\t\t\tuncore.LLC.block[set][way_fill].data = 55;
\t\t\t\t\t\t\t\t\tuncore.LLC.block[set][way_fill].cpu = cpu;
\t\t\t\t\t\t\t\t}

\t\t\t\t\t\t\t\tooo_cpu[cpu].STLB.pagetable_mr_hit_ratio[stat_idx][3]++;
\t\t\t\t\t\t\t\tissue_ptw_dram_read(cpu, addr, instr_id, ip, type, stat_idx);
\t\t\t\t\t\t\t\tcstall += 200;
\t\t\t\t\t\t\t}
\t\t\t\t\t\t}
\t\t\t\t\t}
\t\t\t\t}
\t\t\t}
\t\t}
"""
    with open('src/main.cc', 'w') as f:
        f.writelines(lines[:start_line])
        f.write(new_code)
        f.writelines(lines[end_line:])
else:
    print(f"Could not find markers. Start={start_line}, End={end_line}")
