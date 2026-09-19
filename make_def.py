import re
out = ['LIBRARY UE4SS', 'EXPORTS']
count = 0
with open('exports_raw.txt', encoding='utf-8', errors='replace') as f:
    for line in f:
        # dumpbin /exports line: <ordinal> <hint> <rva> <name> [ = name] [(demangled)]
        m = re.match(r'^\s+(\d+)\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)', line)
        if m:
            name = m.group(2)
            out.append(name)
            count += 1
with open('ue4ss.def', 'w', encoding='ascii') as f:
    f.write('\n'.join(out) + '\n')
print(f'wrote {count} exports to ue4ss.def')