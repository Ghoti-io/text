"""Focused checks for XML profile detection from metadata-free declarations."""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from xml_profile import profile_for


class MetadataFreeProfileDetection(unittest.TestCase):
    def assert_profile(self, data, expected):
        with tempfile.NamedTemporaryFile() as document:
            document.write(data)
            document.flush()
            self.assertEqual(profile_for({}, document.name), expected)

    def test_utf8_bom_declaration_longer_than_512_bytes(self):
        declaration = '<?xml ' + (' ' * 600) + 'version="1.1"?>'
        self.assert_profile(b'\xef\xbb\xbf' + declaration.encode() + b'<r/>', 'XML1.1/NS1.1')

    def test_utf32_bom_declarations_in_both_byte_orders(self):
        declaration = '<?xml version="1.1"?><r/>'
        samples = (
            b'\xff\xfe\x00\x00' + declaration.encode('utf-32-le'),
            b'\x00\x00\xfe\xff' + declaration.encode('utf-32-be'),
        )
        for sample in samples:
            with self.subTest(sample=sample[:4]):
                self.assert_profile(sample, 'XML1.1/NS1.1')

    def test_utf32_without_bom_is_detected(self):
        declaration = '<?xml version="1.1"?><r/>'
        for encoding in ('utf-32-le', 'utf-32-be'):
            with self.subTest(encoding=encoding):
                self.assert_profile(declaration.encode(encoding), 'XML1.1/NS1.1')


if __name__ == '__main__':
    unittest.main()
