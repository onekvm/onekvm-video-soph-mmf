"""Check board dispatch without accessing a bridge or physical memory."""
import os
import re
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOOK = ROOT / 'oe-recipes/recipes-onekvm/onekvm-video-soph-mmf/files/mmf-modules/lt6911uxc-pcie-firmware-sync'


class PcieFirmwareHookTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.hardware = self.work / 'hardware.json'
        self.trace = self.work / 'trace'
        devmem = self.work / 'devmem'
        devmem.write_text('''#!/bin/sh
echo "$*" >>"$TRACE"
case "$1" in
  0x8FFF0000) echo "${MAGIC:-0x44454C4F}" ;;
  0x8FFF0010) echo "${VARIANT:-0x00000001}" ;;
  *) exit 99 ;;
esac
''')
        devmem.chmod(0o755)
        self.hook = self.work / 'hook'
        self.hook.write_text(HOOK.read_text()
                             .replace('/run/onekvm/hardware.json', str(self.hardware))
                             .replace('/dev/i2c-4', str(self.work / 'no-bus'))
                             .replace('/usr/sbin/i2ctransfer', str(self.work / 'no-transfer')))

    def run_hook(self, **env):
        self.trace.unlink(missing_ok=True)
        return subprocess.run(['sh', str(self.hook)], capture_output=True, text=True,
                              env=dict(os.environ, PATH=str(self.work) + ':' + os.environ['PATH'],
                                       TRACE=str(self.trace), **env))

    def test_cube_does_not_read_physical_memory(self):
        self.hardware.write_text('{\n "variant" : "cube", "revision": "beta"\n}')
        result = self.run_hook()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.trace.exists())

    def test_pcie_unknown_and_missing_identity_retain_mailbox_validation(self):
        for identity in ['{"variant":"pcie"}', '{"variant":"unknown"}',
                         '{"variant_label":"cube"}', None]:
            with self.subTest(identity=identity):
                if identity is None:
                    self.hardware.unlink()
                else:
                    self.hardware.write_text(identity)
                result = self.run_hook()
                self.assertEqual(result.returncode, 1)
                self.assertIn('i2c-4 is not available', result.stdout)
                self.assertEqual(self.trace.read_text().splitlines(),
                                 ['0x8FFF0000 32', '0x8FFF0010 32'])

    def test_invalid_mailbox_still_skips(self):
        for env in [dict(MAGIC='0'), dict(VARIANT='0x00000002')]:
            with self.subTest(env=env):
                result = self.run_hook(**env)
                self.assertEqual(result.returncode, 0)
                self.assertNotIn('i2c-4 is not available', result.stdout)

    def enable_bridge(self):
        bus = self.work / 'no-bus'
        bus.touch()
        self.hook.write_text(self.hook.read_text().replace(f'-c {bus}', f'-f {bus}'))
        transfer = self.work / 'no-transfer'
        # Emulate a bridge that returns the installed audited pages. Refuse
        # every programming command so an accidental write fails the test.
        pages = dict(re.findall(r'^expect_([0-9a-f]+)="([^"]+)"', HOOK.read_text(), re.M))
        transfer.write_text('''#!/bin/sh
echo "i2c $*" >>"$TRACE"
case "$4" in
  r1@0x2c) [ -n "${PRESENT_2C-}" ]; exit ;;
  r1@0x44) [ -n "${PRESENT_44-1}" ]; exit ;;
esac
case "$5:$6" in
  0xff:*) printf '%s' "$6" >"$PAGES/bank" ;;
  0x00:r2)
    case $(cat "$PAGES/bank") in
      0xa0) printf '%s\\n' "${C_ID-0x00 0x00}" ;;
      0xe1) printf '%s\\n' "${D_ID-0x00 0x00}"; exit "${D_READ_FAILURE:-0}" ;;
      *) exit 97 ;;
    esac ;;
  0x00:r3) [ "$(cat "$PAGES/bank")" = 0x81 ] || exit 97
    printf '%s\\n' "${CHIP_ID-0x17 0x04 0x83}"; exit "${READ_FAILURE:-0}" ;;
  0x5f:r32) cat "$PAGES/$(cat "$PAGES/hi")$(cat "$PAGES/lo")"; exit ;;
  0x5c:*) printf '%s' "${6#0x}" >"$PAGES/hi" ;;
  0x5d:*) printf '%s' "${6#0x}" >"$PAGES/lo" ;;
  0x59:*|0x08:*|0x5e:0xdf) exit 98 ;;
esac
''')
        transfer.chmod(0o755)
        for name, content in pages.items():
            (self.work / name).write_text(content + '\n')
        (self.work / '0000').write_text('00 ' * 32 + '\n')
        systemctl = self.work / 'systemctl'
        systemctl.write_text('#!/bin/sh\necho "${CORE_STATE:-inactive}"\n')
        systemctl.chmod(0o755)

    def test_non_uxc_and_failed_reads_never_access_flash(self):
        self.enable_bridge()
        # C, D and other UXC revisions are supported by the backend and left
        # unchanged. Unknown silicon or failed reads still fail.
        for env, code, text in [
                (dict(C_ID='0x16 0x05'), 0, 'LT6911C bridge'),
                (dict(CHIP_ID='0xff 0xff 0xff', D_ID='0x21 0x02'), 0, 'LT6911D bridge'),
                (dict(CHIP_ID='', D_ID='0x21 0x02'), 0, 'LT6911D bridge'),
                (dict(CHIP_ID='0x17 0x04 0x84'), 0, 'no audited PCIe image'),
                (dict(CHIP_ID='0x25 0x10 0x23'), 1, 'unknown bridge chip'),
                (dict(CHIP_ID='0x17 0x04'), 1, 'unknown bridge chip'),
                (dict(CHIP_ID='0x00 0x00 0x00', D_ID='0x21 0x02', D_READ_FAILURE='1'), 1, 'unknown bridge chip'),
                (dict(CHIP_ID='0x17 0x04 0x83', READ_FAILURE='1'), 1, 'cannot read chip ID')]:
            with self.subTest(env=env):
                result = self.run_hook(PAGES=str(self.work), **env)
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                self.assertIn(text, result.stdout)
                if text == 'LT6911D bridge' and 'PRESENT_44' not in env:
                    # D is probed only after the UXC window is closed, then its own window closes.
                    calls = self.trace.read_text()
                    self.assertLess(calls.index('0xee 0x00'), calls.index('0xff 0xe0'))
                    self.assertTrue(calls.rstrip().endswith('0xee 0x00'))
                calls = self.trace.read_text()
                self.assertNotIn('0x5a', calls)
                self.assertNotIn('0x5f', calls)
                self.assertNotIn('w33@', calls)

    def test_starting_or_running_core_owns_the_bridge(self):
        self.enable_bridge()
        for state in ['active', 'activating', 'reloading', 'deactivating']:
            with self.subTest(state=state):
                result = self.run_hook(PAGES=str(self.work), CORE_STATE=state)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f'onekvm.service is {state}', result.stdout)
                self.assertNotIn('i2c', self.trace.read_text() if self.trace.exists() else '')

    def test_factory_d_address_map_never_opens_uxc_window(self):
        self.enable_bridge()
        result = self.run_hook(PAGES=str(self.work), PRESENT_44='', D_ID='0x21 0x02')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('LT6911D bridge', result.stdout)
        calls = self.trace.read_text()
        self.assertNotIn('0xff 0x80', calls)
        self.assertNotIn('0xff 0x81', calls)
        self.assertTrue(calls.rstrip().endswith('0xee 0x00'))
        # A non-D chip on that map still gets the C/UXC identification.
        result = self.run_hook(PAGES=str(self.work), PRESENT_44='', C_ID='0x16 0x05')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('LT6911C bridge', result.stdout)
        result = self.run_hook(PAGES=str(self.work), PRESENT_44='', CHIP_ID='0x25 0x10 0x23')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(self.trace.read_text().count('0xff 0xe0'), 2)

    def test_uxc_installed_image_does_not_program_or_reset(self):
        self.enable_bridge()
        result = self.run_hook(PAGES=str(self.work))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('stage6 image already present', result.stdout)
        calls = self.trace.read_text()
        self.assertNotIn('w33@', calls)
        self.assertNotIn('0x03021000', calls)

    def test_unknown_program_is_not_patched_even_if_nor_compatible(self):
        self.enable_bridge()
        # All FF accepts every target byte as a NOR clear; that is not proof
        # this is the audited firmware program.
        (self.work / '1a60').write_text('ff ' * 32 + '\n')
        firmware = self.work / 'unrecognized.bin'
        firmware.write_bytes(bytes(131072))
        dump = self.work / 'dump'
        dump.write_text('#!/bin/sh\ncp "$UNKNOWN_FIRMWARE" "$3"\n')
        dump.chmod(0o755)
        self.hook.write_text(self.hook.read_text()
                             .replace('/usr/libexec/onekvm/lt6911-flash-dump', str(dump))
                             .replace('/var/lib/onekvm/capture-firmware/lt6911uxc',
                                      str(self.work / 'backups')))
        result = self.run_hook(PAGES=str(self.work), UNKNOWN_FIRMWARE=str(firmware))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('unrecognized firmware program', result.stdout)
        self.assertNotIn('w33@', self.trace.read_text())
        self.assertFalse((self.work / 'backups/before-stage6.bin').exists())


if __name__ == '__main__':
    unittest.main()
