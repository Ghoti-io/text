#!/usr/bin/env python3
"""Focused checks for XML 1.1 differential classifications.

Copyright 2026 by Corey Pennycuff
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import xml_classify
import xml_diff


class Xml11LineEndClassification(unittest.TestCase):
    def compare(self, text, reference_text, ours_text="\n"):
        data = ("<?xml version='1.1'?><r>" + text + "</r>").encode("utf-8")
        ours = {"dump": ["S - 72 -", "T " + ours_text.encode("utf-8").hex()]}
        reference = {
            "xml2": {"dump": ["S - 72 -", "T " + reference_text.encode("utf-8").hex()]},
            "expat": {"dump": ["S - 72 -", "T " + reference_text.encode("utf-8").hex()]},
        }
        return xml_classify.xml11_line_end_difference(data, ours, reference, xml_diff.normalize)

    def test_cr_followed_by_line_separator_is_one_line_end(self):
        result = self.compare("\r\u2028", "\n\u2028")
        self.assertIsNotNone(result)
        self.assertEqual(result[0], "reference-wrong")

    def test_cr_followed_by_nel_is_one_line_end(self):
        result = self.compare("\r\u0085", "\n\u0085")
        self.assertIsNotNone(result)
        self.assertEqual(result[0], "reference-wrong")

    def test_standalone_nel_is_normalized(self):
        result = self.compare("\u0085", "\u0085")
        self.assertIsNotNone(result)
        self.assertEqual(result[0], "reference-wrong")

    def test_standalone_nel_does_not_mask_other_difference(self):
        self.assertIsNone(self.compare("\u0085", "\u0085", "wrong"))

    def test_standalone_line_separator_is_normalized(self):
        result = self.compare("\u2028", "\u2028")
        self.assertIsNotNone(result)
        self.assertEqual(result[0], "reference-wrong")

    def test_standalone_line_separator_does_not_mask_other_difference(self):
        self.assertIsNone(self.compare("\u2028", "\u2028", "wrong"))

    def test_unrelated_event_difference_is_not_classified_as_line_end(self):
        self.assertIsNone(self.compare("\r\u2028", "wrong", "unrelated"))


class LegacyEncodingClassification(unittest.TestCase):
    cyrillic = ('<?xml version="1.0" encoding="windows-1251"?><!DOCTYPE a [<!ENTITY e "x">]><a>\u041f</a>'
                ).encode("cp1251")

    def test_a_document_in_a_legacy_encoding_is_read_by_its_label(self):
        text, codec = xml_classify.decode_text(self.cyrillic)
        self.assertEqual(codec, "cp1251")
        self.assertIn("\u041f", text)

    def test_a_label_over_bytes_that_are_not_the_encoding_is_not_read(self):
        self.assertIsNone(xml_classify.decode_text(b'<?xml version="1.0" encoding="shift_jis"?><a>\x82</a>'))

    def test_the_no_dtd_control_keeps_the_encoding_of_the_label(self):
        control = xml_classify.nodtd_control(self.cyrillic)
        self.assertNotIn(b"DOCTYPE", control)
        self.assertTrue(control.endswith("\u041f</a>".encode("cp1251")))

    def test_the_utf8_control_drops_the_label_and_converts(self):
        control = xml_classify.utf8_control(self.cyrillic)
        self.assertIn("\u041f".encode("utf-8"), control)
        self.assertNotIn(b"windows-1251", control)

    def test_a_label_the_encoding_table_has_is_not_scope_unsupported(self):
        self.assertTrue(xml_classify.legacy_codec("Shift_JIS"))
        self.assertIn("windows-1251", xml_classify.whatwg_labels())
        self.assertNotIn("hz-gb-2312", xml_classify.whatwg_labels())
        self.assertNotIn("utf-7", xml_classify.whatwg_labels())


if __name__ == "__main__":
    unittest.main()
