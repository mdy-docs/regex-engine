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

# ECMA-262's property namespaces are disjoint and keyed differently in the
# pattern grammar -- \p{LoneName} accepts only binary properties and
# General_Category values; Script/Script_Extensions values require their
# \p{Key=Value} form -- so every emitted property carries a kind tag and
# lookup is (name, kind), not name alone. See table-binary-unicode-properties
# and table-nonbinary-unicode-properties in the spec; the binary list below
# is that table verbatim (canonical long names). Data-file names outside it
# (contributory properties like Other_Alphabetic, normalization quick-check
# values, etc.) are deliberately NOT emitted -- real engines reject them.
ECMA_BINARY = set("""ASCII ASCII_Hex_Digit Alphabetic Any Assigned Bidi_Control
Bidi_Mirrored Case_Ignorable Cased Changes_When_Casefolded
Changes_When_Casemapped Changes_When_Lowercased Changes_When_NFKC_Casefolded
Changes_When_Titlecased Changes_When_Uppercased Dash
Default_Ignorable_Code_Point Deprecated Diacritic Emoji Emoji_Component
Emoji_Modifier Emoji_Modifier_Base Emoji_Presentation Extended_Pictographic
Extender Grapheme_Base Grapheme_Extend Hex_Digit ID_Continue ID_Start
IDS_Binary_Operator IDS_Trinary_Operator Ideographic Join_Control
Logical_Order_Exception Lowercase Math Noncharacter_Code_Point Pattern_Syntax
Pattern_White_Space Quotation_Mark Radical Regional_Indicator
Sentence_Terminal Soft_Dotted Terminal_Punctuation Unified_Ideograph
Uppercase Variation_Selector White_Space XID_Continue XID_Start""".split())

# table-binary-unicode-properties-of-strings (/v mode only; the C side gates
# on sequence_count > 0 rather than needing a separate kind).
ECMA_STRING = set("""Basic_Emoji Emoji_Keycap_Sequence RGI_Emoji
RGI_Emoji_Flag_Sequence RGI_Emoji_Modifier_Sequence RGI_Emoji_Tag_Sequence
RGI_Emoji_ZWJ_Sequence""".split())

# 1. Aliases. Every alias on a line is a valid spelling in a pattern (the
# spec's UnicodeMatchProperty/UnicodeMatchPropertyValue accept anything in
# PropertyAliases.txt / PropertyValueAliases.txt), so each property is
# emitted once per alias, all sharing one ranges array.
binary_aliases = {}  # canonical long name -> set of every accepted spelling
content = get_ucd_file(UCD_BASE + "PropertyAliases.txt")
for line in content.splitlines():
    line = line.split('#')[0].strip()
    if not line: continue
    parts = [p.strip() for p in line.split(';')]
    if len(parts) >= 2:
        binary_aliases.setdefault(parts[1], set()).update(p for p in parts if p)
binary_canonical = {a: c for c, s in binary_aliases.items() for a in s}

gc_aliases = {}  # canonical long value -> set of every accepted spelling
sc_aliases = {}
content = get_ucd_file(UCD_BASE + "PropertyValueAliases.txt")
for line in content.splitlines():
    line = line.split('#')[0].strip()
    if not line: continue
    parts = [p.strip() for p in line.split(';')]
    if len(parts) >= 3:
        target = gc_aliases if parts[0] == 'gc' else sc_aliases if parts[0] == 'sc' else None
        if target is not None:
            target.setdefault(parts[2], set()).update(p for p in parts[1:] if p)
gc_canonical = {a: c for c, s in gc_aliases.items() for a in s}
sc_canonical = {a: c for c, s in sc_aliases.items() for a in s}

def parse_ranges(url):
    out = []
    for line in get_ucd_file(url).splitlines():
        line = line.split('#')[0].strip()
        if not line: continue
        parts = [p.strip() for p in line.split(';')]
        if len(parts) < 2: continue
        range_str = parts[0]
        if '..' in range_str:
            start, end = (int(x, 16) for x in range_str.split('..'))
        else:
            start = end = int(range_str, 16)
        out.append((start, end, parts[1]))
    return out

# 2. Binary properties (kind BINARY), whitelisted to the ECMA table.
binary_props = {}  # canonical -> [(start, end)]
for url in (UCD_BASE + "DerivedCoreProperties.txt",
            UCD_BASE + "PropList.txt",
            UCD_BASE + "DerivedNormalizationProps.txt",
            UCD_BASE + "extracted/DerivedBinaryProperties.txt",
            UCD_BASE + "emoji/emoji-data.txt"):
    for start, end, name in parse_ranges(url):
        canonical = binary_canonical.get(name, name)
        if canonical in ECMA_BINARY:
            binary_props.setdefault(canonical, []).append((start, end))

# 3. General_Category values (kind GC). DerivedGeneralCategory.txt lists the
# 30 concrete categories; the grouped values (Letter, Mark, ...) are unions
# synthesized here so the C side needs no special cases for them.
gc_props = {}
for start, end, name in parse_ranges(UCD_BASE + "extracted/DerivedGeneralCategory.txt"):
    gc_props.setdefault(gc_canonical.get(name, name), []).append((start, end))
GC_GROUPS = {
    'Letter': ['Lu', 'Ll', 'Lt', 'Lm', 'Lo'],
    'Cased_Letter': ['Lu', 'Ll', 'Lt'],
    'Mark': ['Mn', 'Mc', 'Me'],
    'Number': ['Nd', 'Nl', 'No'],
    'Punctuation': ['Pc', 'Pd', 'Ps', 'Pe', 'Pi', 'Pf', 'Po'],
    'Symbol': ['Sm', 'Sc', 'Sk', 'So'],
    'Separator': ['Zs', 'Zl', 'Zp'],
    'Other': ['Cc', 'Cf', 'Cs', 'Co', 'Cn'],
}
for group, members in GC_GROUPS.items():
    gc_props[group] = [r for m in members for r in gc_props[gc_canonical[m]]]

# Spec-defined synthetics with no data file of their own.
binary_props['Any'] = [(0, 0x10FFFF)]
binary_props['ASCII'] = [(0, 0x7F)]
assigned, cursor = [], 0
for start, end in sorted(gc_props[gc_canonical['Cn']]):
    if start > cursor: assigned.append((cursor, start - 1))
    cursor = max(cursor, end + 1)
if cursor <= 0x10FFFF: assigned.append((cursor, 0x10FFFF))
binary_props['Assigned'] = assigned

# 4. Scripts (kind SCRIPT), as code-point sets because Script_Extensions
# needs set arithmetic below.
script_cps = {}  # canonical -> set of code points
for start, end, name in parse_ranges(UCD_BASE + "Scripts.txt"):
    script_cps.setdefault(sc_canonical.get(name, name), set()).update(range(start, end + 1))

string_properties = {}

# 5. Properties of strings: multi-code-point entries become sequences,
# single-code-point entries become ordinary ranges on the same property.
string_ranges = {}

def parse_emoji_sequences(url):
    content = get_ucd_file(url)
    for line in content.splitlines():
        line = line.split('#')[0].strip()
        if not line: continue
        parts = [p.strip() for p in line.split(';')]
        if len(parts) < 2: continue

        cps_str = parts[0]
        props_to_add = [p for p in (parts[1], "RGI_Emoji") if p in ECMA_STRING]

        if '..' in cps_str:
            start_str, end_str = cps_str.split('..')
            start_seq = [int(x, 16) for x in start_str.split()]
            end_seq = [int(x, 16) for x in end_str.split()]

            if len(start_seq) == 1:
                for p in props_to_add:
                    string_ranges.setdefault(p, []).append((start_seq[0], end_seq[0]))
            else:
                prefix = start_seq[:-1]
                for cp in range(start_seq[-1], end_seq[-1] + 1):
                    for p in props_to_add:
                        string_properties.setdefault(p, []).append(prefix + [cp])
        else:
            seq = [int(x, 16) for x in cps_str.split()]
            for p in props_to_add:
                if len(seq) == 1:
                    string_ranges.setdefault(p, []).append((seq[0], seq[0]))
                else:
                    string_properties.setdefault(p, []).append(seq)

parse_emoji_sequences(EMOJI_URL + "emoji-sequences.txt")
parse_emoji_sequences(EMOJI_URL + "emoji-zwj-sequences.txt")

# 6. Script_Extensions (kind SCX). ScriptExtensions.txt only lists code
# points whose scx differs from their sc; per UAX #24 every unlisted code
# point defaults to scx = { its sc }. The complete set for a script is
# therefore (listed scx members) union (sc members not listed at all) --
# emitting only the file's contents, as this generator originally did,
# produces sets missing their own script's ordinary characters (e.g.
# scx=Greek without alpha/beta/gamma).
scx_listed = {}   # canonical -> set of code points the file assigns it
scx_any = set()   # every code point the file mentions at all
for line in get_ucd_file(UCD_BASE + "ScriptExtensions.txt").splitlines():
    line = line.split('#')[0].strip()
    if not line: continue
    parts = [p.strip() for p in line.split(';')]
    if len(parts) < 2: continue
    range_str = parts[0]
    if '..' in range_str:
        start, end = (int(x, 16) for x in range_str.split('..'))
    else:
        start = end = int(range_str, 16)
    cps = set(range(start, end + 1))
    scx_any |= cps
    for script in parts[1].split():
        scx_listed.setdefault(sc_canonical.get(script, script), set()).update(cps)

scx_props = {}
for canonical in set(script_cps) | set(scx_listed):
    scx_props[canonical] = scx_listed.get(canonical, set()) | (script_cps.get(canonical, set()) - scx_any)

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
print("/* Which \\p{...} namespace an entry answers to -- lookup is (name, kind),")
print(" * matching ECMA-262's grammar: bare \\p{Name} may only be BINARY or GC;")
print(" * SCRIPT/SCX require the \\p{Script=...}/\\p{Script_Extensions=...} form. */")
print("#define UCD_KIND_BINARY 0")
print("#define UCD_KIND_GC     1")
print("#define UCD_KIND_SCRIPT 2")
print("#define UCD_KIND_SCX    3")
print("typedef struct { const char* name; uint8_t kind; const UCDRange* ranges; int count; const UCDStringSequence* sequences; int sequence_count; } UCDProperty;\n")
print("typedef struct { uint32_t from; uint32_t to; } CaseFoldMapping;\n")
print("typedef struct { uint32_t cp; uint32_t mapping; } SimpleCaseMapping;")
print("typedef struct { uint32_t cp; uint8_t lower_len; uint32_t lower[3]; uint8_t title_len; uint32_t title[3]; uint8_t upper_len; uint32_t upper[3]; } SpecialCaseMapping;")
print("typedef struct { uint32_t cp; uint8_t ccc; } CombiningClassMapping;")
print("typedef struct { uint32_t cp; bool is_compat; uint8_t len; uint32_t mapping[18]; } DecompositionMapping;\n")

# Clean up property names to be valid C identifiers
def c_ident(name):
    return name.replace('=', '_').replace('-', '_').replace(' ', '_').replace('.', '_')

def merge_ranges(ranges):
    merged = []
    for r in sorted(ranges):
        if merged and r[0] <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], r[1]))
        else:
            merged.append(r)
    return merged

def cps_to_ranges(cps):
    ranges, run_start, prev = [], None, None
    for cp in sorted(cps):
        if run_start is None:
            run_start = prev = cp
        elif cp == prev + 1:
            prev = cp
        else:
            ranges.append((run_start, prev))
            run_start = prev = cp
    if run_start is not None:
        ranges.append((run_start, prev))
    return ranges

# One ranges/sequences array per (kind, canonical name); one UCD_PROPERTIES
# entry per accepted alias spelling, all sharing that array. entries:
# (spelling, kind macro, array ident, has_ranges, has_seqs)
entries = []

def emit_property(kind_tag, kind_macro, canonical, ranges, seqs, aliases):
    ident = f"{kind_tag}_{c_ident(canonical)}"
    if ranges:
        print(f"static const UCDRange ucd_{ident}_ranges[] = {{")
        for r in ranges:
            print(f"    {{0x{r[0]:04X}, 0x{r[1]:04X}}},")
        print("};\n")
    if seqs:
        print(f"static const UCDStringSequence ucd_{ident}_seqs[] = {{")
        for seq in seqs:
            cps_str = ", ".join(f"0x{cp:04X}" for cp in seq)
            print(f"    {{{{{cps_str}}}, {len(seq)}}},")
        print("};\n")
    for spelling in sorted(aliases | {canonical}):
        entries.append((spelling, kind_macro, ident, bool(ranges), bool(seqs)))

for canonical in sorted(binary_props):
    emit_property("bin", "UCD_KIND_BINARY", canonical, merge_ranges(binary_props[canonical]),
                  None, binary_aliases.get(canonical, set()))
for canonical in sorted(string_properties):
    emit_property("bin", "UCD_KIND_BINARY", canonical, merge_ranges(string_ranges.get(canonical, [])),
                  string_properties[canonical], set())
for canonical in sorted(gc_props):
    emit_property("gc", "UCD_KIND_GC", canonical, merge_ranges(gc_props[canonical]),
                  None, gc_aliases.get(canonical, set()))
for canonical in sorted(script_cps):
    emit_property("sc", "UCD_KIND_SCRIPT", canonical, cps_to_ranges(script_cps[canonical]),
                  None, sc_aliases.get(canonical, set()))
for canonical in sorted(scx_props):
    emit_property("scx", "UCD_KIND_SCX", canonical, cps_to_ranges(scx_props[canonical]),
                  None, sc_aliases.get(canonical, set()))

print("static const UCDProperty UCD_PROPERTIES[] = {")
for spelling, kind_macro, ident, has_ranges, has_seqs in sorted(entries):
    range_ptr = f"ucd_{ident}_ranges" if has_ranges else "NULL"
    range_count = f"sizeof(ucd_{ident}_ranges)/sizeof(UCDRange)" if has_ranges else "0"
    seq_ptr = f"ucd_{ident}_seqs" if has_seqs else "NULL"
    seq_count = f"sizeof(ucd_{ident}_seqs)/sizeof(UCDStringSequence)" if has_seqs else "0"
    print(f"    {{\"{spelling}\", {kind_macro}, {range_ptr}, {range_count}, {seq_ptr}, {seq_count}}},")
print("};\n")

print("static inline const UCDProperty* lookup_unicode_property(const char* name, int kind) {")
print("    for (size_t i = 0; i < sizeof(UCD_PROPERTIES)/sizeof(UCD_PROPERTIES[0]); i++) {")
print("        if (UCD_PROPERTIES[i].kind == kind && strcmp(UCD_PROPERTIES[i].name, name) == 0) return &UCD_PROPERTIES[i];")
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
