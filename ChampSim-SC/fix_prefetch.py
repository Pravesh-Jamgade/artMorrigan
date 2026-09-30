import re

with open('src/main.cc', 'r') as f:
    lines = f.readlines()

start_line = -1
end_line = -1

for i, line in enumerate(lines):
    if "int mmu_cache_prefetch_search(" in line:
        # Search forward for the start of the replacement
        for j in range(i, len(lines)):
            if "if (swap == 0){" in lines[j]:
                start_line = j + 1
                break
    if start_line != -1 and i > start_line:
        if "return mmu_hit[0];" in line: # actually the function returns an int, maybe return mmu_hit[0]? Let's check what it returns
            end_line = i
            break

