#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Text.
#
# Ghoti.io Text is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
#
# Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
# for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""The features a divergence of the condition and rule differentials is listed under (tools/oracle/css_cond_diff.py).

A divergence is abstracted to the smallest thing in the question that explains it, so that a list of a few dozen reasons covers a population of thousands: the first rule whose
pattern is found in the question's text names the feature, and a question no rule names is its own feature (`q:` and its text), which the list then has to name one by one.
Each rule is (feature, pattern, kinds); the order is the order they are tried in.
"""

import re

COND = []
RULE = []


def feature_of(rules, query, values):
    kind = query[0]
    text = "\t".join(query[1:])
    for name, pattern, kinds in rules:
        if kind in kinds and re.search(pattern, text, re.I | re.S):
            return name
    return "q:%s:%s" % (kind, text.replace("\n", " ")[:160])
