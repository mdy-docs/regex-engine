import urllib.request
import sys
import os

CACHE_DIR = ".ucd_cache"

if not os.path.exists(CACHE_DIR):
    os.makedirs(CACHE_DIR)

# Unicode version to generate against. Bump this (and re-run) to update ucd.h.
UNICODE_VERSION = "17.0.0"
# Emoji sequence files (emoji-sequences.txt / emoji-zwj-sequences.txt) are
# released on their own cadence under Public/emoji/<ver>/. There is no emoji/17.0
# release, so pin the latest published set. (emoji-data.txt lives inside the UCD
# tree and does track UNICODE_VERSION.)
EMOJI_SEQ_VERSION = "16.0"

UCD_BASE = f"https://www.unicode.org/Public/{UNICODE_VERSION}/ucd/"
EMOJI_URL = f"https://www.unicode.org/Public/emoji/{EMOJI_SEQ_VERSION}/"

def get_ucd_file(url):
    # Key the cache on the full URL path so different Unicode/Emoji versions do
    # not collide under the same bare filename (which would silently reuse stale
    # data after a version bump).
    key = url.replace("https://www.unicode.org/Public/", "").replace("/", "_")
    filepath = os.path.join(CACHE_DIR, key)
    if os.path.exists(filepath):
        print(f"// Using cached {filepath}...", file=sys.stderr)
        with open(filepath, 'r', encoding='utf-8') as f:
            return f.read()
    print(f"// Fetching {url}...", file=sys.stderr)
    req = urllib.request.urlopen(url)
    content = req.read().decode('utf-8')
    with open(filepath, 'w', encoding='utf-8') as f:
        f.write(content)
    return content

# 1. Fetch aliases
property_aliases = {} # short -> long
content = get_ucd_file(UCD_BASE + "PropertyAliases.txt")
for line in content.splitlines():
    line = line.split('#')[0].strip()
    if not line: continue
    parts = [p.strip() for p in line.split(';')]
    if len(parts) >= 2:
        property_aliases[parts[0]] = parts[1]

prop_value_aliases = {} # (property, short_val) -> long_val
content = get_ucd_file(UCD_BASE + "PropertyValueAliases.txt")
for line in content.splitlines():
    line = line.split('#')[0].strip()
    if not line: continue
    parts = [p.strip() for p in line.split(';')]
    if len(parts) >= 3:
        prop = parts[0]
        if prop in ('gc', 'sc', 'scx'):
            prop_value_aliases[parts[1]] = parts[2]
            prop_value_aliases[parts[2]] = parts[2]

UCD_URLS = [
    UCD_BASE + "DerivedCoreProperties.txt",
    UCD_BASE + "Scripts.txt",
    UCD_BASE + "extracted/DerivedGeneralCategory.txt",
    UCD_BASE + "PropList.txt",
    UCD_BASE + "DerivedNormalizationProps.txt",
]

properties = {}

for url in UCD_URLS:
    content = get_ucd_file(url)
    for line in content.splitlines():
        line = line.split('#')[0].strip()
        if not line: continue
        parts = [p.strip() for p in line.split(';')]
        if len(parts) < 2: continue
        
        range_str, prop_name = parts[0], parts[1]
        
        # Only process known binary properties or specific properties we want
        # Actually, let's keep it simple: normalize the property name.
        if prop_name in prop_value_aliases:
            prop_name = prop_value_aliases[prop_name]
        elif prop_name in property_aliases:
            prop_name = property_aliases[prop_name]
            
        if '..' in range_str:
            start, end = range_str.split('..')
            start, end = int(start, 16), int(end, 16)
        else:
            start = end = int(range_str, 16)
            
        if prop_name not in properties:
            properties[prop_name] = []
        properties[prop_name].append((start, end))

# Handle emoji-data
emoji_content = get_ucd_file(UCD_BASE + "emoji/emoji-data.txt")
for line in emoji_content.splitlines():
    line = line.split('#')[0].strip()
    if not line: continue
    parts = [p.strip() for p in line.split(';')]
    if len(parts) < 2: continue
    range_str, prop_name = parts[0], parts[1]
    
    if '..' in range_str:
        start, end = range_str.split('..')
        start, end = int(start, 16), int(end, 16)
    else:
        start = end = int(range_str, 16)
        
    if prop_name not in properties:
        properties[prop_name] = []
    properties[prop_name].append((start, end))

string_properties = {}

def parse_emoji_sequences(url):
    content = get_ucd_file(url)
    for line in content.splitlines():
        line = line.split('#')[0].strip()
        if not line: continue
        parts = [p.strip() for p in line.split(';')]
        if len(parts) < 2: continue
        
        cps_str = parts[0]
        prop_name = parts[1]
        
        props_to_add = [prop_name, "RGI_Emoji"]
        
        if '..' in cps_str:
            start_str, end_str = cps_str.split('..')
            start_seq = [int(x, 16) for x in start_str.split()]
            end_seq = [int(x, 16) for x in end_str.split()]
            
            if len(start_seq) == 1:
                for p in props_to_add:
                    if p not in properties: properties[p] = []
                    properties[p].append((start_seq[0], end_seq[0]))
            else:
                prefix = start_seq[:-1]
                for cp in range(start_seq[-1], end_seq[-1] + 1):
                    for p in props_to_add:
                        if p not in string_properties: string_properties[p] = []
                        string_properties[p].append(prefix + [cp])
        else:
            seq = [int(x, 16) for x in cps_str.split()]
            for p in props_to_add:
                if len(seq) == 1:
                    if p not in properties: properties[p] = []
                    properties[p].append((seq[0], seq[0]))
                else:
                    if p not in string_properties: string_properties[p] = []
                    string_properties[p].append(seq)

parse_emoji_sequences(EMOJI_URL + "emoji-sequences.txt")
parse_emoji_sequences(EMOJI_URL + "emoji-zwj-sequences.txt")

# Handle ScriptExtensions.txt
scx_content = get_ucd_file(UCD_BASE + "ScriptExtensions.txt")
for line in scx_content.splitlines():
    line = line.split('#')[0].strip()
    if not line: continue
    parts = [p.strip() for p in line.split(';')]
    if len(parts) < 2: continue
    range_str = parts[0]
    scripts = parts[1].split()
    
    if '..' in range_str:
        start, end = range_str.split('..')
        start, end = int(start, 16), int(end, 16)
    else:
        start = end = int(range_str, 16)
        
    for script in scripts:
        long_script = prop_value_aliases.get(script, script)
        prop_name = f"Script_Extensions={long_script}"
        if prop_name not in properties:
            properties[prop_name] = []
        properties[prop_name].append((start, end))

UCD_CASE_FOLD_URL = UCD_BASE + "CaseFolding.txt"
case_folds = []
content = get_ucd_file(UCD_CASE_FOLD_URL)
for line in content.splitlines():
    line = line.split('#')[0].strip()
    if not line: continue
    parts = [p.strip() for p in line.split(';')]
    if len(parts) >= 3:
        if parts[1] in ('C', 'S'):
            case_folds.append((int(parts[0], 16), int(parts[2], 16)))
case_folds.sort()

# Parse UnicodeData.txt
unicode_data = get_ucd_file(UCD_BASE + "UnicodeData.txt")
simple_uppercase = []
simple_lowercase = []
ccc_list = []
decompositions = []
for line in unicode_data.splitlines():
    line = line.split('#')[0].strip()
    if not line: continue
    parts = line.split(';')
    if len(parts) < 14: continue
    cp = int(parts[0], 16)
    ccc = int(parts[3])
    if ccc != 0:
        ccc_list.append((cp, ccc))
    
    decomp = parts[5].strip()
    if decomp:
        is_compat = decomp.startswith('<')
        d_parts = decomp.split(' ')[1:] if is_compat else decomp.split(' ')
        d_cps = [int(x, 16) for x in d_parts if x]
        decompositions.append((cp, is_compat, d_cps))
        
    uc = parts[12].strip()
    lc = parts[13].strip()
    if uc: simple_uppercase.append((cp, int(uc, 16)))
    if lc: simple_lowercase.append((cp, int(lc, 16)))

# Parse CompositionExclusions.txt
comp_exclusions = []
exclusions_data = get_ucd_file(UCD_BASE + "CompositionExclusions.txt")
for line in exclusions_data.splitlines():
    line = line.split('#')[0].strip()
    if not line: continue
    comp_exclusions.append(int(line, 16))

# Parse SpecialCasing.txt
special_cases = []
special_casing_data = get_ucd_file(UCD_BASE + "SpecialCasing.txt")
for line in special_casing_data.splitlines():
    line = line.split('#')[0].strip()
    if not line: continue
    parts = [p.strip() for p in line.split(';')]
    if len(parts) >= 4:
        if len(parts) > 4 and parts[4]: continue # Skip conditional context-dependent mappings
        cp = int(parts[0], 16)
        lower = [int(x, 16) for x in parts[1].split()]
        title = [int(x, 16) for x in parts[2].split()]
        upper = [int(x, 16) for x in parts[3].split()]
        special_cases.append((cp, lower, title, upper))

# Output C Header
print("#ifndef UCD_H")
print("#define UCD_H\n")
print("#include <stdint.h>")
print("#include <stdbool.h>")
print("#include <string.h>\n")
print("typedef struct { uint32_t start; uint32_t end; } UCDRange;")
print("typedef struct { uint32_t cps[16]; int length; } UCDStringSequence;")
print("typedef struct { const char* name; const UCDRange* ranges; int count; const UCDStringSequence* sequences; int sequence_count; } UCDProperty;\n")
print("typedef struct { uint32_t from; uint32_t to; } CaseFoldMapping;\n")
print("typedef struct { uint32_t cp; uint32_t mapping; } SimpleCaseMapping;")
print("typedef struct { uint32_t cp; uint8_t lower_len; uint32_t lower[3]; uint8_t title_len; uint32_t title[3]; uint8_t upper_len; uint32_t upper[3]; } SpecialCaseMapping;")
print("typedef struct { uint32_t cp; uint8_t ccc; } CombiningClassMapping;")
print("typedef struct { uint32_t cp; bool is_compat; uint8_t len; uint32_t mapping[18]; } DecompositionMapping;\n")

# Clean up property names to be valid C identifiers
def c_ident(name):
    return name.replace('=', '_').replace('-', '_').replace(' ', '_').replace('.', '_')

prop_names = sorted(set(list(properties.keys()) + list(string_properties.keys())))

for prop in prop_names:
    if prop in properties:
        ranges = sorted(properties[prop])
        merged = []
        for r in ranges:
            if not merged: merged.append(r)
            else:
                if r[0] <= merged[-1][1] + 1: merged[-1] = (merged[-1][0], max(merged[-1][1], r[1]))
                else: merged.append(r)
                
        print(f"static const UCDRange ucd_prop_{c_ident(prop)}_ranges[] = {{")
        for r in merged:
            print(f"    {{0x{r[0]:04X}, 0x{r[1]:04X}}},")
        print("};\n")

    if prop in string_properties:
        print(f"static const UCDStringSequence ucd_prop_{c_ident(prop)}_seqs[] = {{")
        for seq in string_properties[prop]:
            cps_str = ", ".join(f"0x{cp:04X}" for cp in seq)
            print(f"    {{{{{cps_str}}}, {len(seq)}}},")
        print("};\n")

print("static const UCDProperty UCD_PROPERTIES[] = {")
for prop in prop_names:
    range_ptr = f"ucd_prop_{c_ident(prop)}_ranges" if prop in properties else "NULL"
    range_count = f"sizeof(ucd_prop_{c_ident(prop)}_ranges)/sizeof(UCDRange)" if prop in properties else "0"
    seq_ptr = f"ucd_prop_{c_ident(prop)}_seqs" if prop in string_properties else "NULL"
    seq_count = f"sizeof(ucd_prop_{c_ident(prop)}_seqs)/sizeof(UCDStringSequence)" if prop in string_properties else "0"
    print(f"    {{\"{prop}\", {range_ptr}, {range_count}, {seq_ptr}, {seq_count}}},")
print("};\n")

print("static inline const UCDProperty* lookup_unicode_property(const char* name) {")
print("    for (size_t i = 0; i < sizeof(UCD_PROPERTIES)/sizeof(UCD_PROPERTIES[0]); i++) {")
print("        if (strcmp(UCD_PROPERTIES[i].name, name) == 0) return &UCD_PROPERTIES[i];")
print("    }\n    return NULL;\n}")

print(f"\nstatic const CaseFoldMapping UCD_CASE_FOLD[] = {{")
for mapping in case_folds:
    print(f"    {{0x{mapping[0]:04X}, 0x{mapping[1]:04X}}},")
print("};\n")

print("static inline uint32_t unicode_casefold(uint32_t cp) {")
print("    int left = 0, right = sizeof(UCD_CASE_FOLD)/sizeof(CaseFoldMapping) - 1;")
print("    while (left <= right) {")
print("        int mid = left + (right - left) / 2;")
print("        if (UCD_CASE_FOLD[mid].from == cp) return UCD_CASE_FOLD[mid].to;")
print("        if (UCD_CASE_FOLD[mid].from < cp) left = mid + 1;")
print("        else right = mid - 1;")
print("    }")
print("    return cp;")
print("}")

print("\nstatic const SimpleCaseMapping UCD_SIMPLE_UPPERCASE[] = {")
for cp, m in simple_uppercase:
    print(f"    {{0x{cp:04X}, 0x{m:04X}}},")
print("};\n")

print("\nstatic const SimpleCaseMapping UCD_SIMPLE_LOWERCASE[] = {")
for cp, m in simple_lowercase:
    print(f"    {{0x{cp:04X}, 0x{m:04X}}},")
print("};\n")

print("\nstatic const CombiningClassMapping UCD_COMBINING_CLASS[] = {")
for cp, ccc in ccc_list:
    print(f"    {{0x{cp:04X}, {ccc}}},")
print("};\n")

print("\nstatic const DecompositionMapping UCD_DECOMPOSITION[] = {")
for cp, is_compat, cps in decompositions:
    cps_str = ", ".join(f"0x{x:04X}" for x in cps)
    print(f"    {{0x{cp:04X}, {'true' if is_compat else 'false'}, {len(cps)}, {{{cps_str}}}}},")
print("};\n")

print("\nstatic const uint32_t UCD_COMPOSITION_EXCLUSIONS[] = {")
for cp in sorted(comp_exclusions):
    print(f"    0x{cp:04X},")
print("};\n")

print("\nstatic const SpecialCaseMapping UCD_SPECIAL_CASING[] = {")
# SpecialCasing.txt is not in codepoint order; sort so the C side can binary-search.
special_cases.sort(key=lambda e: e[0])
for cp, lower, title, upper in special_cases:
    lower_str = ", ".join(f"0x{x:04X}" for x in lower)
    title_str = ", ".join(f"0x{x:04X}" for x in title)
    upper_str = ", ".join(f"0x{x:04X}" for x in upper)
    print(f"    {{0x{cp:04X}, {len(lower)}, {{{lower_str}}}, {len(title)}, {{{title_str}}}, {len(upper)}, {{{upper_str}}}}},")
print("};\n")

print("\n#endif /* UCD_H */")
