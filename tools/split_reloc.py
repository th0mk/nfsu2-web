"""Relocate statically lifted operands, not already compiled guest bytes.

The native MIT patch scans .text for these addresses. Here the disassembler
has already separated operands from instructions, so only operands move.
Macros retain the original two-slot layout for native builds.
"""

SETTINGS_READERS = {
    0x41e23e, 0x41e245, 0x41e28d, 0x497d59, 0x4da0f1, 0x4da8da,
    0x5ea7dd, 0x5ea8c7, 0x5eb406, 0x5eb426, 0x5f17e9, 0x5f17f2,
    0x5f2a93, 0x5f2fb6, 0x5f2fc8, 0x5f325f, 0x5f32aa, 0x5f33ce,
    0x5f33dd, 0x5f3402, 0x5f345e, 0x5f3a33, 0x5f6388, 0x5ffe31,
    0x6001fb, 0x600497, 0x60353d,
}

def operand(value, ip=0, immediate=False):
    value &= 0xffffffff
    if ip in SETTINGS_READERS and 0x83aa64 <= value < 0x83aab4:
        return f'split_settings_address(0x{value:x}u)'
    if immediate and value == 0x64000 and ip in (0x48cd61, 0x48cd6b, 0x48cd90, 0x48cd9c):
        return '(SPLIT_PORT_CAPACITY > 2u ? 0x100000u : 0x64000u)'
    if immediate and ip == 0x5cb720 and value == 1:
        return 'split_render_limit()'
    # The teardown's indexed load is inOrder[-1], NOT byIndex[1].
    if ip == 0x60aa73 and value == 0x8900b0:
        return '(SPLIT_PLAYERS_ORDER - 4u)'
    if 0x8900ac <= value < 0x8900c0:
        if value < 0x8900b4:
            base, offset = 'SPLIT_PLAYERS_INDEX', value - 0x8900ac
        elif value < 0x8900bc:
            base, offset = 'SPLIT_PLAYERS_ORDER', value - 0x8900b4
        else:
            base, offset = 'SPLIT_PLAYERS_COUNT', value - 0x8900bc
        return f'({base} + {offset}u)'
    if 0x8655a0 <= value < 0x865930:
        return f'(SPLIT_PORT_TABLES + {value - 0x8655a0}u)'
    # 0x865930 also names the next global: only these comparisons are ends.
    if immediate and value == 0x865930 and ip in (0x57b3aa, 0x580ad2):
        return 'SPLIT_PORT_TABLE_END'
    if immediate and value == 2 and ip in (0x4856c7, 0x77d9ca, 0x782295):
        return 'SPLIT_PORT_CAPACITY'
    if immediate and value == 0xa0 and ip == 0x579558:
        return '(0x50u * SPLIT_PORT_CAPACITY)'
    return f'0x{value:x}u'
