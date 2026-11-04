"""Verify split-screen operand sites against the user's original executable.

Run with the build environment's capstone and pefile, before regenerating.
This reads the executable; it never patches or redistributes game bytes.
"""
import sys
import capstone
import pefile
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM
from split_reloc import SETTINGS_READERS

pe = pefile.PE(sys.argv[1])
base = pe.OPTIONAL_HEADER.ImageBase
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
decoder.detail = True

def operands(address):
    insn = next(decoder.disasm(pe.get_data(address - base, 16), address))
    return [op.imm if op.type == X86_OP_IMM else op.mem.disp
            for op in insn.operands if op.type in (X86_OP_IMM, X86_OP_MEM)]

for address in SETTINGS_READERS:
    values = operands(address)
    assert any(0x83aa64 <= v < 0x83aab4 for v in values), (hex(address), values)
for address in (0x48cd61, 0x48cd6b, 0x48cd90, 0x48cd9c):
    assert 0x64000 in operands(address), hex(address)
assert 2 in operands(0x4856c7)
print(f'Validated {len(SETTINGS_READERS)} settings readers, four scratch sizes and the human manager capacity.')
