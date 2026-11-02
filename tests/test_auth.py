"""Offline regression tests. No BMW account, broker or network is used."""
import base64
import hashlib
import json
import os
from pathlib import Path
import shlex
import stat
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

FAKE_CURL = r'''#!/usr/bin/env python3
import json, os, pathlib, sys
fixture = pathlib.Path(os.environ['BMW_AUTH_TEST_FIXTURE'])
data = json.loads(fixture.read_text())
args = sys.argv[1:]
output = args[args.index('--output') + 1]
form = [args[i+1] for i, arg in enumerate(args) if arg == '--data-urlencode']
log = fixture.with_suffix('.requests')
with log.open('a') as file:
    file.write(json.dumps({'url': args[-1], 'form': dict(x.split('=', 1) for x in form), 'args': args}) + '\n')
if args[-1].endswith('/device/code'):
    response = data['device']
else:
    counter = fixture.with_suffix('.count')
    number = int(counter.read_text()) if counter.exists() else 0
    response = data['tokens'][number]
    counter.write_text(str(number + 1))
body = response['body']
pathlib.Path(output).write_text(body if isinstance(body, str) else json.dumps(body))
print(response.get('status', 200), end='')
sys.exit(response.get('exit', 0))
'''


def jwt(payload):
    encoded = base64.urlsafe_b64encode(json.dumps(payload).encode()).decode().rstrip('=')
    return 'e30.' + encoded + '.c2ln'


class DeviceFlowTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='bmw-auth-test-')
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.state = self.directory / 'state' / 'bmw-mqtt-bridge'
        self.state.mkdir(parents=True)
        (self.state / '.env').write_text(
            'CLIENT_ID=22222222-2222-2222-2222-222222222222\n'
            'GCID=33333333-3333-3333-3333-333333333333\n')
        self.original = {name: 'PREVIOUS_' + name for name in ('id_token', 'refresh_token', 'access_token')}
        self.bin = self.directory / 'bin'
        self.bin.mkdir()
        (self.bin / 'curl').write_text(FAKE_CURL)
        (self.bin / 'curl').chmod(0o755)
        (self.bin / 'sleep').write_text('#!/bin/sh\nprintf "%s\\n" "$1" >> "$BMW_AUTH_TEST_FIXTURE.sleeps"\n')
        (self.bin / 'sleep').chmod(0o755)
        self.fixture = self.directory / 'fixture.json'
        self.environment = os.environ.copy()
        self.environment.update({
            'PATH': str(self.bin) + os.pathsep + os.environ['PATH'],
            'XDG_STATE_HOME': str(self.state.parent),
            'BMW_AUTH_TEST_FIXTURE': str(self.fixture),
        })
        self.valid = {'id_token': jwt({'exp': int(time.time()) + 3600}),
                      'refresh_token': 'NEW_REFRESH_SECRET', 'access_token': 'NEW_ACCESS_SECRET',
                      'expires_in': 3600}
        self.device = {'device_code': 'TEST_DEVICE_SECRET', 'user_code': 'TEST1234',
                       'verification_uri_complete': 'https://example.invalid/authorize',
                       'interval': 5, 'expires_in': 300}

    def run_flow(self, tokens, device=None):
        for name, value in self.original.items():
            (self.state / (name + '.txt')).write_text(value)
        for path in self.directory.glob('fixture.*'):
            path.unlink()
        self.fixture.write_text(json.dumps({'device': device or {'body': self.device}, 'tokens': tokens}))
        result = subprocess.run(['/bin/bash', str(ROOT / 'scripts/bmw_flow.sh')], input='\n',
                                text=True, capture_output=True, env=self.environment, timeout=10)
        self.assertFalse(list(self.state.glob('.auth.*')), 'temporary credentials were left behind')
        output = result.stdout + result.stderr
        for secret in ('NEW_ACCESS_SECRET', 'NEW_REFRESH_SECRET', 'TEST_DEVICE_SECRET', self.valid['id_token']):
            self.assertNotIn(secret, output)
        return result

    def assert_preserved(self):
        for name, value in self.original.items():
            self.assertEqual((self.state / (name + '.txt')).read_text(), value)

    def test_invalid_token_responses_preserve_existing_tokens(self):
        cases = [{}, [], None, '<html>Bad Gateway</html>',
                 {'fault': {'faultstring': 'Invalid Access Token',
                            'detail': {'errorcode': 'keymanagement.service.invalid_access_token'}}}]
        for key in ('access_token', 'id_token', 'refresh_token'):
            for value in (None, 'null', '', 42, True, 'bad token', 'line\nbreak'):
                cases.append(dict(self.valid, **{key: value}))
            body = dict(self.valid)
            del body[key]
            cases.append(body)
        for value in (None, '3600', False, 0, -1, 1.5):
            cases.append(dict(self.valid, expires_in=value))
        for payload in ({}, [], {'exp': None}, {'exp': 'tomorrow'}, {'exp': True},
                        {'exp': int(time.time())-1}):
            cases.append(dict(self.valid, id_token=jwt(payload)))
        for value in ('not-a-jwt', '.e30.sig', 'e30.e30.', 'e30.e30.sig.extra'):
            cases.append(dict(self.valid, id_token=value))
        cases.extend([dict(self.valid, error='invalid_grant'),
                      dict(self.valid, fault={'faultstring': 'Invalid Access Token'})])
        for body in cases:
            with self.subTest(body=body):
                result = self.run_flow([{'body': body}])
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn('✔', result.stdout)
                self.assert_preserved()

    def test_http_errors_and_network_failures_preserve_existing_tokens(self):
        for status, exit_code in ((400, 0), (401, 0), (429, 0), (500, 0), (0, 28)):
            with self.subTest(status=status, exit_code=exit_code):
                result = self.run_flow([{'body': self.valid, 'status': status, 'exit': exit_code}])
                self.assertNotEqual(result.returncode, 0)
                self.assert_preserved()

    def test_invalid_device_responses_preserve_existing_tokens(self):
        bodies = [{}, {'error': 'invalid_request'}, {'fault': {'faultstring': 'Invalid Access Token'}},
                  dict(self.device, device_code=None), dict(self.device, interval='5'),
                  dict(self.device, interval=-1), dict(self.device, interval=False),
                  dict(self.device, expires_in=0),
                  dict(self.device, verification_uri_complete='javascript:alert(1)')]
        for body in bodies:
            with self.subTest(body=body):
                result = self.run_flow([], device={'body': body})
                self.assertNotEqual(result.returncode, 0)
                self.assert_preserved()

    def test_valid_response_and_pkce(self):
        result = self.run_flow([{'body': self.valid}])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Tokens validated and saved', result.stdout)
        for name in self.original:
            path = self.state / (name + '.txt')
            self.assertEqual(path.read_text().strip(), self.valid[name])
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        requests = [json.loads(line) for line in self.fixture.with_suffix('.requests').read_text().splitlines()]
        first, poll = requests
        self.assertEqual(first['form']['response_type'], 'device_code')
        verifier = poll['form']['code_verifier']
        self.assertRegex(verifier, r'^[A-Za-z0-9._~-]{43,128}$')
        challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip('=')
        self.assertEqual(first['form']['code_challenge'], challenge)
        for request in requests:
            self.assertIn('--connect-timeout', request['args'])
            self.assertIn('--max-time', request['args'])

    def test_pending_and_slow_down_continue_polling(self):
        result = self.run_flow([{'body': {'error': 'authorization_pending'}, 'status': 400},
                                {'body': {'error': 'slow_down'}, 'status': 400}, {'body': self.valid}])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(Path(str(self.fixture) + '.sleeps').read_text().splitlines(), ['5', '10'])


class RefreshTests(unittest.TestCase):
    def test_cpp_refresh_transport_and_persistence(self):
        with tempfile.TemporaryDirectory(prefix='bmw-refresh-test-') as directory:
            binary = Path(directory) / 'test-refresh'
            pkg_flags = shlex.split(subprocess.check_output(
                ['pkg-config', '--cflags', '--libs', 'libmosquitto'], text=True))
            command = ['g++', '-std=c++17', '-O0', '-pthread', str(ROOT / 'tests/test_token_refresh.cpp'),
                       '-o', str(binary), *pkg_flags, '-lcurl', '-Wl,--wrap=curl_easy_setopt',
                       '-Wl,--wrap=curl_easy_perform', '-Wl,--wrap=curl_easy_getinfo',
                       '-Wl,--wrap=rename']
            build = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary), str(Path(directory) / 'state')],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS:', result.stdout)
            for secret in ('NEW_REFRESH_SECRET', 'NEW_ACCESS_SECRET', 'OLD_REFRESH_SECRET'):
                self.assertNotIn(secret, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
