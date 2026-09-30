import re

with open('src/main.cc', 'r') as f:
    lines = f.readlines()

start_line = -1
end_line = -1

for i, line in enumerate(lines):
    if "int mmu_cache_prefetch_search(" in line:
        for j in range(i, len(lines)):
            if "if (swap == 0){" in lines[j]:
                start_line = j + 1
                # Skip the LRU updates
                for k in range(start_line, len(lines)):
                    if "uint64_t cr3 = 0x200000;" in lines[k]:
                        start_line = k
                        break
                break
        
        for j in range(start_line, len(lines)):
            if "return cstall;" in lines[j]:
                end_line = j - 1 # previous line should be '}' closing the if(swap == 0) block
                while lines[end_line].strip() != '}':
                    end_line -= 1
                break
        break

if start_line != -1 and end_line != -1:
    new_code = """\t\tuint64_t cr3 = 0x200000;
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
\t\tuint32_t set;
\t\tint way_read;
\t\tPACKET search_packet;

\t\tfor (int lvl = walk_depth - 1; lvl >= 0; --lvl) {
\t\t\tint mapped_mmu_idx = (walk_depth > 3 && lvl >= 1 && lvl <= 3) ? (3 - lvl) : -1;
\t\t\tif (walk_depth == 3 && lvl >= 1 && lvl <= 2) mapped_mmu_idx = 2 - lvl;
\t\t\t
\t\t\tbool hit = false;
\t\t\tif (mapped_mmu_idx >= 0 && mapped_mmu_idx < 3) {
\t\t\t\thit = mmu_hit[mapped_mmu_idx];
\t\t\t}
\t\t\tif (walk_depth > 4 && lvl >= 4) hit = false;

\t\t\tint stat_idx = 3 - lvl;
\t\t\tif (stat_idx < 0) stat_idx = 0;
\t\t\tif (stat_idx > 3) stat_idx = 3;

\t\t\tif (!hit) {
\t\t\t\tuint64_t addr = level_addrs[lvl];
\t\t\t\tsearch_packet.address = addr >> LOG2_BLOCK_SIZE;
\t\t\t\tsearch_packet.full_addr = addr;

\t\t\t\tset = ooo_cpu[cpu].L1D.get_set(addr >> LOG2_BLOCK_SIZE);
\t\t\t\tway_read = PTW_START_LEVEL == 1 ? ooo_cpu[cpu].L1D.check_hit(&search_packet) : -1;

\t\t\t\tif (way_read >= 0) {
\t\t\t\t\tooo_cpu[cpu].L1D.mark_translation_access(set, way_read, addr, stat_idx);
\t\t\t\t\tif (lvl != 0 || iflag) ooo_cpu[cpu].STLB.pagetable_mr_hit_ratio[stat_idx][0]++;
\t\t\t\t\tcstall += PTW_START_LEVEL == 1 ? L1D_LATENCY : 0;
\t\t\t\t} else {
\t\t\t\t\tcstall += PTW_START_LEVEL == 1 ? L1D_LATENCY : 0;
\t\t\t\t\t
\t\t\t\t\tset = ooo_cpu[cpu].L2C.get_set(addr >> LOG2_BLOCK_SIZE);
\t\t\t\t\tway_read = ooo_cpu[cpu].L2C.check_hit(&search_packet);

\t\t\t\t\tif (way_read >= 0) {
\t\t\t\t\t\tooo_cpu[cpu].L2C.mark_translation_access(set, way_read, addr, stat_idx);
\t\t\t\t\t\tif (lvl != 0 || iflag) ooo_cpu[cpu].STLB.pagetable_mr_hit_ratio[stat_idx][1]++;
\t\t\t\t\t\tcstall += L2C_LATENCY;
\t\t\t\t\t} else {
\t\t\t\t\t\tcstall += L2C_LATENCY;

\t\t\t\t\t\tset = uncore.LLC.get_set(addr >> LOG2_BLOCK_SIZE);
\t\t\t\t\t\tway_read = uncore.LLC.check_hit(&search_packet);

\t\t\t\t\t\tif (way_read >= 0) {
\t\t\t\t\t\t\tuncore.LLC.mark_translation_access(set, way_read, addr, stat_idx);
\t\t\t\t\t\t\tif (lvl != 0 || iflag) ooo_cpu[cpu].STLB.pagetable_mr_hit_ratio[stat_idx][2]++;
\t\t\t\t\t\t\tcstall += LLC_LATENCY;
\t\t\t\t\t\t} else {
\t\t\t\t\t\t\tcstall += LLC_LATENCY;
\t\t\t\t\t\t\tcstall += 200;
\t\t\t\t\t\t\tif (lvl != 0 || iflag) ooo_cpu[cpu].STLB.pagetable_mr_hit_ratio[stat_idx][3]++;
\t\t\t\t\t\t\tissue_ptw_dram_read(cpu, addr, 0, ip, 0, stat_idx);
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
    print("Replaced successfully.")
else:
    print(f"Could not find markers. Start={start_line}, End={end_line}")
