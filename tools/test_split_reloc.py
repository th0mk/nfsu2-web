import unittest
from split_reloc import operand

class SplitRelocations(unittest.TestCase):
    def test_render_scratch_only_at_verified_sites(self):
        for ip in (0x48cd61, 0x48cd6b, 0x48cd90, 0x48cd9c):
            self.assertIn('0x100000u', operand(0x64000, ip, True))
            self.assertEqual(operand(0x64000, ip, False), '0x64000u')
        self.assertEqual(operand(0x64000, 0, True), '0x64000u')

    def test_player_arrays(self):
        self.assertEqual(operand(0x8900ac), '(SPLIT_PLAYERS_INDEX + 0u)')
        self.assertEqual(operand(0x8900b0), '(SPLIT_PLAYERS_INDEX + 4u)')
        self.assertEqual(operand(0x8900b4), '(SPLIT_PLAYERS_ORDER + 0u)')
        self.assertEqual(operand(0x8900bc), '(SPLIT_PLAYERS_COUNT + 0u)')
        self.assertEqual(operand(0x8900c0), '0x8900c0u')

    def test_teardown_uses_order_minus_one(self):
        self.assertEqual(operand(0x8900b0, 0x60aa73), '(SPLIT_PLAYERS_ORDER - 4u)')
        self.assertEqual(operand(0x8900b0, 0x60aa00), '(SPLIT_PLAYERS_INDEX + 4u)')

    def test_end_address_is_also_unrelated_global(self):
        for ip in (0x57b3aa, 0x580ad2):
            self.assertEqual(operand(0x865930, ip, True), 'SPLIT_PORT_TABLE_END')
            self.assertEqual(operand(0x865930, ip, False), '0x865930u')
        self.assertEqual(operand(0x865930, 0, True), '0x865930u')

    def test_counts_change_only_at_verified_sites(self):
        self.assertEqual(operand(2, 0x4856c7, True), 'SPLIT_PORT_CAPACITY')
        self.assertEqual(operand(2, 0x77d9ca, True), 'SPLIT_PORT_CAPACITY')
        self.assertEqual(operand(2, 0x782295, True), 'SPLIT_PORT_CAPACITY')
        self.assertEqual(operand(2, 0x782295, False), '0x2u')
        self.assertEqual(operand(0xa0, 0x579558, True), '(0x50u * SPLIT_PORT_CAPACITY)')
        self.assertEqual(operand(0xa0, 0x579558, False), '0xa0u')

    def test_settings_readers_preserve_other_globals(self):
        self.assertEqual(operand(0x83aa64, 0x5f2a93), 'split_settings_address(0x83aa64u)')
        self.assertEqual(operand(0x83aab4, 0x5f2a93), '0x83aab4u')
        self.assertEqual(operand(0x83aa64, 0), '0x83aa64u')

if __name__ == '__main__':
    unittest.main()
