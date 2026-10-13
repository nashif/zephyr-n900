# Notes: hardware enablement in your own repository

Working notes for a blog post on maintaining the Nokia N900 port outside
the Zephyr tree, using Zephyr's out-of-tree features. Goal: find the gaps.

## Starting point

- `topic/n900/bringup` in zephyr: 57 commits off upstream/main, written as
  if for upstream: arch, SoC, board, ~20 drivers, bindings, tests.
- Out-of-tree apps in `~/zephyrproject/n900/*` (phone demo, camera,
  audio, bring-up diagnostics), built against that branch.
- Module template: upstream `example-application` (commit 38a6d9b).

## Step 1: classify every commit

Rule: anything that only *adds* files is a candidate for the module; any
change to *existing* core code stays in a zephyr branch.

- 42 commits only add files (plus one-line registrations in core
  `CMakeLists.txt`/`Kconfig` lists that the module replaces with its own).
- 15 commits change existing core code -> `topic/n900/core`:
  - arch: Cortex-A8 CPU symbol, MMU shareability option, page-table clean
    before TLB invalidate
  - cmake: gcc `-mfpu` table entry for the A8 (plus an indentation-only
    prerequisite)
  - existing drivers: omap_mcspi (CS polarity, OMAP2/3 layout), tsl2561
    (TSL2563, fractional lux, glass attenuation), gpio_keys (switches)
  - subsystems: input switch events and key codes, LVGL software rotation
  - tests for the above

Observation: the CPU itself can never live out of tree. A new core needs
the arch Kconfig symbol, the compiler `-mcpu`/`-mfpu` mapping and (here)
MMU fixes; none of that has an extension point.

## Step 2: the zephyr branch

`topic/n900/core` = upstream/main + the 15 core commits, cherry-picked
cleanly (no conflicts, since they never touched the new files). The
module's `west.yml` points zephyr at that branch on a fork: the first
cost of the approach is that "out of tree" still means "on a fork" as
soon as one core change is needed, with a rebase per upstream update.

## Step 3: the module

Started from example-application, deleted the examples, kept the layout.
`zephyr/module.yml` sets `board_root`, `soc_root`, `dts_root` and lists
the west runner. Files kept the paths they had in the Zephyr tree
(`drivers/<class>/...`, `dts/bindings/<class>/...`,
`include/zephyr/drivers/mfd/twl4030.h`,
`include/zephyr/dt-bindings/pinctrl/ti-omap3-pinctrl.h`), so moving them
back upstream later is a plain copy.

What carried over without changes:
- SoC (`soc/ti/omap3`, family `ti_omap`) and board (`boards/nokia/n900`)
- the OMAP3430 dtsi, 21 bindings, the dt-bindings header (dts_root
  include paths cover `<root>/include`)
- 26 driver sources; Kconfig `select`s of core symbols
  (`DISPLAY_FRAMEBUFFER`, `TICKLESS_CAPABLE`,
  `UDC_DRIVER_HAS_HIGH_SPEED_SUPPORT`) just work
- the public MFD header under `include/zephyr/...`
- the west runner (`runners:` in module.yml): `west flash --context`
  lists `nolo` as the board's default runner
- twister: `ZEPHYR_EXTRA_MODULES` is enough for it to find the module
  board; all 25 apps build with `-p n900`

Result: the phone demo built from the module is byte-for-byte the same
size as from the in-tree branch (text 880636, bss 15413222).

What needed glue:
- One `zephyr_library()` per driver class plus `if <SUBSYSTEM>` guards in
  the module Kconfig, replicating the structure of `drivers/` in core.
- `bq27200/CMakeLists.txt` relied on the parent's library in core; the
  module version needs its own `zephyr_library()`.
- The I2C driver's Kconfig symbol had been added to an existing core file
  (`drivers/i2c/Kconfig.omap`); it got its own file.

## Gaps found

1. **New CPUs cannot be out of tree.** The Cortex-A8 needs an arch
   Kconfig symbol, `-mcpu`/`-mfpu` table entries in `cmake/` and here
   MMU fixes. No extension point exists for any of them.
2. **Fixes to in-tree drivers cannot be overlaid.** omap_mcspi, tsl2561
   and gpio_keys changes stay on the fork. A module cannot replace an
   in-tree driver for the same compatible without duplicate symbols; the
   only alternative is forking the driver under a new compatible.
3. **Subsystem extensions are core.** New input event types and key codes
   live in public headers; the LVGL glue is in `modules/lvgl`.
4. **Private driver-class headers.** Four drivers include headers that
   are not part of the public include path:
   - `udc_common.h` (every USB device controller driver is built on it)
   - `system_timer_generic.h` (the generic system timer core)
   - `i2c-priv.h` (`i2c_map_dt_bitrate()`)
   - `display_framebuffer.h` (framebuffer display helper)
   Workaround: `zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/<class>)`.
   It works, but ties the module to internals with no stability promise.
5. **Noise from empty core driver libraries.** When the board enables a
   driver class whose only drivers come from the module (charger, fuel
   gauge, MFD, LED, GPIO, audio, console), each core `drivers__<class>`
   library has no sources and CMake prints "No SOURCES given to Zephyr
   library" - seven warnings per phone build. Harmless, but looks broken.
6. **Test coverage does not follow the drivers.** The core `build_all`
   tests had entries for five of these drivers; out of tree they are
   gone, so the module needs its own build test. Writing it immediately
   found a real bug that the in-tree branch still has: the TWL4030 MFD
   driver's ISR is unused when the node has no interrupt, an error under
   `-Werror`. In tree, nothing ever built it that way.
7. **Runner test fixtures are not reusable.** The NOLO runner test uses
   fixtures from `scripts/west_commands/tests/conftest.py`; the module's
   conftest has to load that file by path from `ZEPHYR_BASE`.
8. **Board documentation is orphaned.** `boards/nokia/n900/doc/index.rst`
   no longer feeds any doc build; the module has no Sphinx setup.
9. **Kconfig menu placement.** Module drivers show up under Modules, not
   next to their class under Device Drivers.
10. **Compliance tooling assumes the Zephyr tree.** Zephyr's
    `check_compliance.py` runs on any git repository, and most checks
    passed from the module (BoardYml, KconfigHWMv2, DevicetreeBindings,
    CMakeStyle, KeepSorted, KconfigBasic). But:
    - its configuration files (`.checkpatch.conf`, `.gitlint`,
      `.clang-format`, ...) are looked up in the checked repository, so
      without copies checkpatch silently applies Linux defaults (80
      columns) and gitlint does not require Signed-off-by;
    - `.gitlint` points at Zephyr's `scripts/gitlint` rules, so a copied
      config fails to load until those rules are copied too;
    - the Kconfig check reports every module symbol as undefined unless
      `ZEPHYR_EXTRA_MODULES` names the module, and still does not parse
      application Kconfig files;
    - SysbuildKconfig crashes: its `git grep` for `SB_CONFIG_` finds
      nothing in the module and exits 1, which the script treats as an
      error.

What worked well and is worth saying in the post:
- module.yml roots for boards, SoCs and devicetree, and module runners
- twister finding a module board through `ZEPHYR_EXTRA_MODULES` only
- identical binaries in and out of tree

## How much is left for upstream

Core branch `topic/n900/core`: 15 commits, 25 files, +455/-99 lines.
All of it is generic and a reasonable upstream candidate on its own:
Cortex-A8 support, two Armv7 MMU fixes, the A8 `-mfpu` mapping, McSPI
OMAP2/3 layout and CS polarity, TSL2563 plus fractional lux and glass
attenuation, input switch events and camera key codes, gpio_keys
switches, LVGL software rotation.

Module: about 9,300 lines of port (SoC 855, board 824, devicetree 815,
drivers 6,435, headers 179, runner 92, tests 188) plus 5,900 lines of
apps and 600 of tools. Much of the driver code is not N900-specific
and would be useful upstream: the TWL4030 family (also on the
BeagleBoard), TLV320AIC3x, LIS302DL, LP5523, ADP1653, BQ27200,
BQ24150A, TSC2005, the OMAP3 I2C, McBSP and MUSB drivers.

The point for the post: keeping the port out of tree does not remove the
need for upstream, it shrinks it. The 15 core commits are the ones that
must be upstreamed (or carried on a fork forever); everything else can
be upstreamed later, one driver at a time, or never.
