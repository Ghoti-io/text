#!/usr/bin/env python3
"""Generate TOML v1.0.0 documents, and what each one means.

Written for `toml_diff.py`, whose whole point is that the fuzzers check this
library against itself and the corpus holds the cases somebody chose. A
generator is the third thing: documents nobody chose, in a shape a second
implementation can be asked about.

Two properties matter more than variety:

**Every document is valid v1.0.0.** An invalid one would be a comparison of two
error messages, which says nothing - both implementations refusing is not
agreement about anything. The generator therefore builds a *value tree* first and
spells it afterwards, so validity is a property of the construction rather than
something to be checked.

**The spelling varies independently of the value.** A key may be bare, quoted or
dotted; a table may be a `[header]`, a sub-header or inline; a string may be
basic, literal or multi-line of either; an array may be on one line or several.
That is where a reader's disagreements live: two documents with the same values
and different spellings are the interesting pair, and a generator that spelled
each value one way would be testing one path.

Seeded, so a disagreement can be reproduced from the line the gate prints.

Copyright 2026 by Corey Pennycuff
"""

import os
import random
import string
import sys

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "conformance"))
from toml_native import normalise_datetime  # noqa: E402

BARE = string.ascii_letters + string.digits + "_-"

# Characters a basic string has to escape, and the ones worth putting in on
# purpose: a quote, a backslash, a control character, a non-BMP codepoint, and
# the two the spec singles out.
INTERESTING = [
    '"', '\\', '\n', '\t', '\r', '\x00', '\x7f', 'é', '́',
    ' ', '﻿', '\U0001f600', "'", '#', '=', '[', ']', '.', ' ',
]


class Gen:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.lines = []
        self.indent = ''

    # --- values ----------------------------------------------------------
    def integer(self):
        pick = self.rng.random()
        if pick < 0.15:
            return self.rng.choice([0, -0, 1, -1,
                                    9223372036854775807,
                                    -9223372036854775808])
        return self.rng.randint(-10 ** self.rng.randint(1, 18),
                                10 ** self.rng.randint(1, 18))

    def flt(self):
        pick = self.rng.random()
        if pick < 0.2:
            return self.rng.choice([0.0, -0.0, 1.0, -1.0, 0.5,
                                    float('inf'), float('-inf'),
                                    float('nan')])
        return self.rng.uniform(-1e6, 1e6)

    def text(self):
        n = self.rng.randint(0, 12)
        out = []
        for _ in range(n):
            if self.rng.random() < 0.3:
                out.append(self.rng.choice(INTERESTING))
            else:
                out.append(self.rng.choice(string.printable[:62]))
        return ''.join(out)

    def scalar(self):
        """A (python value, spelling) pair."""
        pick = self.rng.randrange(6)
        if pick == 0:
            v = self.integer()
            return v, self.spell_integer(v)
        if pick == 1:
            v = self.flt()
            return v, self.spell_float(v)
        if pick == 2:
            v = self.rng.choice([True, False])
            return v, 'true' if v else 'false'
        if pick == 3:
            v = self.text()
            return v, self.spell_string(v)
        if pick == 4:
            return self.datetime_value()
        return self.array()

    # --- spellings -------------------------------------------------------
    def spell_integer(self, value):
        if value >= 0 and self.rng.random() < 0.2:
            return self.rng.choice(['0x%x' % value, '0o%o' % value,
                                    bin(value)])
        if abs(value) >= 1000 and self.rng.random() < 0.2:
            # Underscores, which a reader has to strip and not read as digits.
            digits = str(abs(value))
            cut = self.rng.randrange(1, len(digits))
            spelt = digits[:cut] + '_' + digits[cut:]
            return ('-' if value < 0 else '') + spelt
        return str(value)

    def spell_float(self, value):
        if value != value:
            return self.rng.choice(['nan', '+nan', '-nan'])
        if value == float('inf'):
            return self.rng.choice(['inf', '+inf'])
        if value == float('-inf'):
            return '-inf'
        # repr() round-trips a double exactly, which is the property this needs;
        # `%e` would not. A whole number needs the point TOML requires.
        spelt = repr(value)
        if 'e' not in spelt and '.' not in spelt:
            spelt += '.0'
        return spelt

    @staticmethod
    def literal_can_hold(value):
        """A literal string escapes nothing, so it cannot hold a quote, a
        newline, or any control character but tab."""
        if "'" in value:
            return False
        return all(ch == '\t' or (0x20 <= ord(ch) and ord(ch) != 0x7F)
                   for ch in value)

    def spell_string(self, value):
        style = self.rng.randrange(4)
        if style == 1 and self.literal_can_hold(value):
            return "'" + value + "'"
        if style == 2:
            # Multi-line basic. The newline straight after the opening
            # delimiter is trimmed, so the body is the value.
            return '"""\n' + self.basic_body(value) + '"""'
        return self.basic(value)

    def basic_body(self, value):
        out = []
        for ch in value:
            if ch == '"':
                out.append('\\"')
            elif ch == '\\':
                out.append('\\\\')
            elif ch == '\n':
                out.append('\\n')
            elif ch == '\r':
                out.append('\\r')
            elif ch == '\t':
                out.append('\\t')
            elif ord(ch) < 0x20 or ord(ch) == 0x7F:
                out.append('\\u%04X' % ord(ch))
            elif ord(ch) > 0xFFFF and self.rng.random() < 0.5:
                out.append('\\U%08X' % ord(ch))
            else:
                out.append(ch)
        return ''.join(out)

    def basic(self, value):
        return '"' + self.basic_body(value) + '"'

    def datetime_value(self):
        year = self.rng.randint(1, 9999)
        month = self.rng.randint(1, 12)
        day = self.rng.randint(1, 28)
        hour, minute, second = (self.rng.randint(0, 23),
                                self.rng.randint(0, 59),
                                self.rng.randint(0, 59))
        frac = ''
        if self.rng.random() < 0.4:
            frac = '.' + ''.join(self.rng.choice(string.digits)
                                 for _ in range(self.rng.randint(1, 6)))
        kind = self.rng.randrange(4)
        if kind == 0:
            spelt = '%04d-%02d-%02d' % (year, month, day)
        elif kind == 1:
            spelt = '%02d:%02d:%02d%s' % (hour, minute, second, frac)
        elif kind == 2:
            sep = self.rng.choice(['T', 't', ' '])
            spelt = '%04d-%02d-%02d%s%02d:%02d:%02d%s' % (
                year, month, day, sep, hour, minute, second, frac)
        else:
            # `-00:00` is deliberately absent. v1.0.0 gives it the meaning
            # "offset unknown", and the reference cannot hold that: Python's
            # fromisoformat reads it as UTC and isoformat writes `+00:00` back,
            # so generating one would produce a disagreement about the
            # reference's model rather than about either reader.
            offset = self.rng.choice(['Z', 'z', '+00:00', '+05:30', '-08:00'])
            spelt = '%04d-%02d-%02dT%02d:%02d:%02d%s%s' % (
                year, month, day, hour, minute, second, frac, offset)
        # The value is the spelling in canonical form: both sides are asked to
        # read it and both are compared through normalise_datetime(), so the
        # generator need not model a calendar - only agree about the spelling.
        return ('date-time', normalise_datetime(spelt)), spelt

    def array(self, depth=0):
        n = self.rng.randint(0, 4)
        values, spellings = [], []
        for _ in range(n):
            if depth < 2 and self.rng.random() < 0.2:
                v, s = self.array(depth + 1)
            else:
                v, s = self.simple_scalar()
            values.append(v)
            spellings.append(s)
        if self.rng.random() < 0.3 and n:
            # Multi-line, with the trailing comma v1.0.0 already allows.
            body = ',\n  '.join(spellings)
            return values, '[\n  ' + body + ',\n]'
        return values, '[' + ', '.join(spellings) + ']'

    def simple_scalar(self):
        """A scalar that is not an array, for array elements."""
        pick = self.rng.randrange(5)
        if pick == 0:
            v = self.integer()
            return v, self.spell_integer(v)
        if pick == 1:
            v = self.flt()
            return v, self.spell_float(v)
        if pick == 2:
            v = self.rng.choice([True, False])
            return v, 'true' if v else 'false'
        if pick == 3:
            v = self.text()
            return v, self.spell_string(v)
        return self.datetime_value()

    # --- keys ------------------------------------------------------------
    def key(self, used):
        for _ in range(64):
            style = self.rng.randrange(3)
            if style == 0:
                name = ''.join(self.rng.choice(BARE)
                               for _ in range(self.rng.randint(1, 8)))
                spelt = name
            elif style == 1:
                name = self.text() or 'k'
                spelt = self.basic(name)
            else:
                name = ''.join(self.rng.choice(BARE + ' .')
                               for _ in range(self.rng.randint(1, 8))) or 'k'
                spelt = self.basic(name)
            if name and name not in used:
                used.add(name)
                return name, spelt
        raise RuntimeError('ran out of key names')

    # --- documents -------------------------------------------------------
    def table(self, path, depth):
        """A table's values, emitting its lines. Returns the value dict.

        **Every key-value line comes before the first header.** A `[header]`
        moves the table every later key-value line belongs to, so a line emitted
        after one is not this table's any more - which is how an inline table
        chosen in the second loop used to be written into the last header's table
        while being recorded here. Both readers agreed with the text and
        disagreed with the generator, which is the generator being wrong in the
        one way that would have read as a finding.
        """
        out = {}
        used = set()
        # The key-value part, scalars and inline tables together: both are
        # `key = value` lines and neither may follow a header.
        for _ in range(self.rng.randint(0, 4)):
            name, spelt = self.key(used)
            if depth < 2 and self.rng.random() < 0.25:
                value, text = self.inline_table(depth + 1)
            else:
                value, text = self.scalar()
            self.lines.append('%s = %s' % (spelt, text))
            out[name] = value
        if depth >= 2:
            return out
        # And then the headers, each of which takes the rest of its own lines
        # with it.
        for _ in range(self.rng.randint(0, 2)):
            name, spelt = self.key(used)
            if self.rng.random() < 0.5:
                self.lines.append('')
                self.lines.append('[%s]' % '.'.join(path + [spelt]))
                out[name] = self.table(path + [spelt], depth + 1)
            else:
                # An array of tables, which is the one construct with no inline
                # spelling at all.
                items = []
                for _ in range(self.rng.randint(1, 2)):
                    self.lines.append('')
                    self.lines.append('[[%s]]' % '.'.join(path + [spelt]))
                    items.append(self.table(path + [spelt], depth + 1))
                out[name] = items
        return out

    def inline_table(self, depth):
        out = {}
        used = set()
        parts = []
        for _ in range(self.rng.randint(0, 3)):
            name, spelt = self.key(used)
            if depth < 3 and self.rng.random() < 0.2:
                value, text = self.inline_table(depth + 1)
            else:
                value, text = self.simple_scalar()
            out[name] = value
            parts.append('%s = %s' % (spelt, text))
        return out, '{' + ', '.join(parts) + '}'

    def document(self):
        """(text, values) for one document."""
        self.lines = []
        if self.rng.random() < 0.3:
            self.lines.append('# a comment, which neither reader keeps')
        values = self.table([], 0)
        text = '\n'.join(self.lines)
        if text and not text.endswith('\n'):
            text += '\n'
        return text, values


def documents(seed, count):
    """`count` (seed, text, values) triples, each reproducible from its seed."""
    for i in range(count):
        gen = Gen(seed + i)
        text, values = gen.document()
        yield seed + i, text, values


if __name__ == "__main__":
    import sys
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    for s, text, _values in documents(seed, count):
        print("# --- seed %d ---" % s)
        sys.stdout.write(text)
