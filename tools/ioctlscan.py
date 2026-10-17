# Scan a MIPS ELF for ioctl-number-like constants (lui+ori/addiu) with a given type char.
import sys, capstone, collections
from elftools.elf.elffile import ELFFile
elf = ELFFile(open(sys.argv[1], 'rb')); t = ord(sys.argv[2])
text = elf.get_section_by_name('.text')
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
md.detail = False; md.skipdata = True
last_lui = {}; found = collections.Counter(); where = collections.defaultdict(list)
for i in md.disasm(text.data(), text['sh_addr']):
    ops = [o.strip() for o in i.op_str.split(',')]
    if i.mnemonic in ('li',) and len(ops)==2:
        v=int(ops[1],0)&0xffffffff
        if ((v>>8)&0xff)==t: found[v]+=1; where[v].append(hex(i.address))
    if i.mnemonic == 'lui': last_lui[ops[0]] = (int(ops[1], 0), i.address)
    elif i.mnemonic in ('ori', 'addiu') and len(ops) == 3 and ops[1] in last_lui and i.address - last_lui[ops[1]][1] < 40:
        imm = int(ops[2], 0)
        v = ((last_lui[ops[1]][0] << 16) + (imm if i.mnemonic == 'ori' else imm)) & 0xffffffff
        if ((v >> 8) & 0xff) == t and (v >> 30) in (1, 2, 3) and ((v >> 16) & 0x3fff) < 0x200:
            found[v] += 1; where[v].append(hex(i.address))
    elif i.mnemonic in ('addiu','ori') and len(ops) == 3 and ops[1] == '$zero':
        v = int(ops[2], 0) & 0xffff
        if (v >> 8) == t: found[v] += 1; where[v].append(hex(i.address))
for v, n in sorted(found.items()):
    d = {1: 'W', 2: 'R', 3: 'WR'}.get(v >> 30, '-')
    print(f"0x{v:08x} _IO{d}('{chr(t)}', 0x{v & 0xff:02x}, size={(v >> 16) & 0x3fff})  x{n}  {where[v][:4]}")
