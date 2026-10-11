# SPDX-License-Identifier: BSD-3-Clause
"""Gives the package the version in include/crnet/crnet.h, so that it is
written down once. Everything else is in pyproject.toml."""

import pathlib
import re

from setuptools import setup

HERE = pathlib.Path(__file__).resolve().parent


def version():
    header = HERE.parents[1] / "include" / "crnet" / "crnet.h"
    if header.is_file():
        text = header.read_text()
        return ".".join(
            re.search(r"^#define CRNET_VERSION_%s (\d+)$" % part, text,
                      re.M).group(1)
            for part in ("MAJOR", "MINOR", "PATCH"))
    # A source distribution has no header, but its PKG-INFO, written
    # when it was made from the tree, has the version.
    for line in (HERE / "PKG-INFO").read_text().splitlines():
        if line.startswith("Version: "):
            return line.split(None, 1)[1]
    raise RuntimeError("no version in PKG-INFO")


setup(version=version())
