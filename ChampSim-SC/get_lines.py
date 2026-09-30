with open('src/main.cc', 'r') as f:
    lines = f.readlines()

for i, line in enumerate(lines):
    if "int mmu_cache_prefetch_search(" in line:
        print(f"Start: {i}")
    if "return mmu_hit" in line:
        print(f"End roughly: {i}")
