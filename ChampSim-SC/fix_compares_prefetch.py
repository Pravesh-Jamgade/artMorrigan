import re

with open('src/main.cc', 'r') as f:
    content = f.read()

# We need to find the specific block in mmu_cache_prefetch_search
# It looks like:
# uint64_t compare_pml4, compare_pdp, compare_pd;
# uint64_t cstall = 2;
# ooo_cpu[cpu].STLB.mmu_timer++;
# mmu_hit[0] = ...

old_code = """\tuint64_t compare_pml4, compare_pdp, compare_pd;

\tuint64_t cstall = 2;
\tooo_cpu[cpu].STLB.mmu_timer++;

\tmmu_hit[0]"""

new_code = """\tuint64_t compare_pml4, compare_pdp, compare_pd;
\tuint64_t ptes_per_page = PAGE_SIZE / 8;
\tuint64_t log2_ptes = lg2(ptes_per_page);
\tcompare_pml4 = vpage >> (3 * log2_ptes);
\tcompare_pdp  = vpage >> (2 * log2_ptes);
\tcompare_pd   = vpage >> (1 * log2_ptes);

\tuint64_t cstall = 2;
\tooo_cpu[cpu].STLB.mmu_timer++;

\tmmu_hit[0]"""

if old_code in content:
    content = content.replace(old_code, new_code)
    with open('src/main.cc', 'w') as f:
        f.write(content)
    print("Replaced successfully.")
else:
    print("Could not find old_code")
