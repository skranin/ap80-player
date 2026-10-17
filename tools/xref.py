# Find code that references given strings in a MIPS32 ELF; dump nearby instructions.
import sys, capstone
from elftools.elf.elffile import ELFFile
path = sys.argv[1]; pats = sys.argv[2:]
data = open(path, 'rb').read(); elf = ELFFile(open(path, 'rb'))
segs = [(s['p_offset'], s['p_vaddr'], s['p_filesz']) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
def va(off):
    for o, v, n in segs:
        if o <= off < o + n: return v + off - o
text = elf.get_section_by_name('.text'); base = text['sh_addr']; code = text.data()
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
ins = list(md.disasm(code, base))
idx = {i.address: k for k, i in enumerate(ins)}
for p in pats:
    off = data.find(p.encode()); start = data.rfind(b'\0', 0, off) + 1; a = va(start)
    hi, lo = (a + 0x8000) >> 16, a & 0xffff
    lo_s = lo - 0x10000 if lo >= 0x8000 else lo
    print(f"== {p!r} va=0x{a:x}")
    for k, i in enumerate(ins):
        if i.mnemonic == 'addiu' and i.op_str.endswith(f", {lo_s}") or i.mnemonic == 'addiu' and i.op_str.endswith(f", 0x{lo:x}") or (i.mnemonic=='addiu' and i.op_str.endswith(hex(lo_s))):
            win = ins[max(0, k - 30):k + 2]
            if any(w.mnemonic == 'lui' and w.op_str.endswith(hex(hi)) for w in win):
                for w in win: print(f"  {w.address:08x} {w.mnemonic} {w.op_str}")
                print("  ----")
