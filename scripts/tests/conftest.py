# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

# Reuse the runner test fixtures of Zephyr's west commands, which are not
# packaged for out-of-tree runners: load their conftest by path.

import importlib.util
import os
import sys
from pathlib import Path

ZEPHYR_BASE = Path(os.environ['ZEPHYR_BASE'])
WEST_COMMANDS = ZEPHYR_BASE / 'scripts' / 'west_commands'

sys.path.insert(0, str(WEST_COMMANDS))
sys.path.insert(0, str(Path(__file__).parents[1] / 'runners'))

_spec = importlib.util.spec_from_file_location('zephyr_runner_conftest',
                                               WEST_COMMANDS / 'tests' / 'conftest.py')
_core = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_core)

RC_KERNEL_BIN = _core.RC_KERNEL_BIN
runner_config = _core.runner_config
