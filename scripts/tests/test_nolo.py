# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

import argparse
import os
from unittest.mock import call, patch

from conftest import RC_KERNEL_BIN
from nolo import NoloBinaryRunner

os_path_isfile = os.path.isfile


def os_path_isfile_patch(filename):
    if filename == RC_KERNEL_BIN:
        return True
    return os_path_isfile(filename)


@patch('runners.core.ZephyrBinaryRunner.require')
@patch('runners.core.ZephyrBinaryRunner.check_call')
def test_nolo_flash(cc, req, runner_config):
    parser = argparse.ArgumentParser(allow_abbrev=False)
    NoloBinaryRunner.add_parser(parser)
    runner = NoloBinaryRunner.create(runner_config, parser.parse_args(['--flasher', 'flasher']))
    with patch('os.path.isfile', side_effect=os_path_isfile_patch):
        runner.run('flash')
    req.assert_called_with('flasher')
    assert cc.call_args_list == [call(['flasher', '-m', f'kernel:{RC_KERNEL_BIN}', '-l', '-b'])]
