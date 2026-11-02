"""Verify compile.sh returns failures instead of reporting a successful CI job."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BuildScriptTests(unittest.TestCase):
    def run_build(self, compiler_status=0, pkg_config_status=0):
        with tempfile.TemporaryDirectory(prefix='bmw-build-test-') as temporary:
            directory = Path(temporary)
            (directory / 'scripts').mkdir()
            (directory / 'src').mkdir()
            shutil.copyfile(ROOT / 'scripts/compile.sh', directory / 'scripts/compile.sh')
            tools = directory / 'bin'
            tools.mkdir()
            pkg_config = tools / 'pkg-config'
            pkg_config.write_text(
                '#!/bin/sh\n'
                f'if [ {pkg_config_status} -ne 0 ]; then exit {pkg_config_status}; fi\n'
                'printf "%s\\n" "-I/test/include -lmosquitto"\n')
            pkg_config.chmod(0o755)
            compiler = tools / 'g++'
            compiler.write_text(
                '#!/bin/sh\n'
                'printf "%s\\n" "$@" > "$BUILD_TEST_ARGUMENTS"\n'
                f'exit {compiler_status}\n')
            compiler.chmod(0o755)
            arguments = directory / 'compiler-arguments'
            environment = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'],
                               BUILD_TEST_ARGUMENTS=str(arguments))
            result = subprocess.run(['/bin/bash', str(directory / 'scripts/compile.sh')],
                                    capture_output=True, text=True, env=environment, timeout=10)
            return result, arguments.read_text().splitlines() if arguments.exists() else None

    def test_compiler_failure_is_a_failure(self):
        result, _ = self.run_build(compiler_status=42)
        self.assertEqual(result.returncode, 42)
        self.assertNotIn('Build successful', result.stdout)

    def test_pkg_config_failure_stops_before_compiling(self):
        result, arguments = self.run_build(pkg_config_status=37)
        self.assertEqual(result.returncode, 37)
        self.assertIsNone(arguments)
        self.assertNotIn('Build successful', result.stdout)

    def test_success_passes_separate_pkg_config_arguments(self):
        result, arguments = self.run_build()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Build successful', result.stdout)
        self.assertIn('-I/test/include', arguments)
        self.assertIn('-lmosquitto', arguments)
