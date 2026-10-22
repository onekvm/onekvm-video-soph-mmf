"""Check board dispatch without accessing a bridge or physical memory."""
import os
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


if __name__ == '__main__':
    unittest.main()
