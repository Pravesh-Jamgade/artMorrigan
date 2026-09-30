import re

with open('src/main.cc', 'r') as f:
    content = f.read()

old_code = """\tif(LOG2_PAGE_SIZE == 12){
\t\tcompare_pml4 = vpage>>27; 
\t\tcompare_pdp  = vpage>>18; 
\t\tcompare_pd   = vpage>>9; 
\t}
\telse{
\t\tcompare_pml4 = vpage>>18; 
\t\tcompare_pdp  = vpage>>9; 
\t}"""

new_code = """\tuint64_t ptes_per_page = PAGE_SIZE / 8;
\tuint64_t log2_ptes = lg2(ptes_per_page);
\tcompare_pml4 = vpage >> (3 * log2_ptes);
\tcompare_pdp  = vpage >> (2 * log2_ptes);
\tcompare_pd   = vpage >> (1 * log2_ptes);"""

if old_code in content:
    content = content.replace(old_code, new_code)
    with open('src/main.cc', 'w') as f:
        f.write(content)
    print("Replaced successfully.")
else:
    print("Could not find the old code.")
