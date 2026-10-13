# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Runner for the Nokia NOLO bootloader, using the 0xFFFF flasher."""

import argparse

from runners.core import RunnerCaps, RunnerConfig, ZephyrBinaryRunner


class NoloBinaryRunner(ZephyrBinaryRunner):
    """Loads an image into RAM through NOLO and boots it."""

    def __init__(self, cfg: RunnerConfig, flasher: str):
        super().__init__(cfg)
        self.flasher = flasher

    @classmethod
    def name(cls):
        return 'nolo'

    @classmethod
    def capabilities(cls) -> RunnerCaps:
        return RunnerCaps(commands={'flash'})

    @classmethod
    def do_add_parser(cls, parser: argparse.ArgumentParser):
        parser.add_argument('--flasher', default='0xFFFF', help='path to the 0xFFFF flasher')

    @classmethod
    def do_create(cls, cfg: RunnerConfig, args: argparse.Namespace):
        return NoloBinaryRunner(cfg, flasher=args.flasher)

    def do_run(self, command: str, **kwargs):
        self.require(self.flasher)
        self.ensure_output('bin')

        self.logger.info('waiting for the device in NOLO USB mode')
        self.check_call([self.flasher, '-m', f'kernel:{self.cfg.bin_file}', '-l', '-b'])
