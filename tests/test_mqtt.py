"""Exercise local MQTT client IDs with real bridge processes and Mosquitto."""
import base64
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
CLIENT_IDS = ('aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa',
              'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb')


class LocalMqttTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix='bmw-mqtt-binary-')
        cls.addClassCleanup(temporary.cleanup)
        cls.binary = Path(temporary.name) / 'bmw_mqtt_bridge'
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'libmosquitto'], text=True))
        subprocess.run(['g++', '-std=c++17', '-O0', '-pthread',
                        str(ROOT / 'src/bmw_mqtt_bridge.cpp'), '-o', str(cls.binary),
                        *flags, '-lcurl'], check=True, capture_output=True, text=True,
                       timeout=120)

    def start_process(self, command, environment, log):
        output = log.open('w')
        self.addCleanup(output.close)
        process = subprocess.Popen(command, env=environment, stdout=output,
                                   stderr=subprocess.STDOUT)

        def stop():
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)

        self.addCleanup(stop)
        return process

    def wait_for(self, predicate, processes, logs, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for process in processes:
                self.assertIsNone(process.poll(), '\n'.join(log.read_text() for log in logs))
            if predicate():
                return
            time.sleep(0.05)
        self.fail('Timed out waiting for MQTT connections:\n' +
                  '\n'.join(log.read_text() for log in logs))

    def check_clients(self, prefixes, overrides, client_ids=CLIENT_IDS):
        temporary = tempfile.TemporaryDirectory(prefix='bmw-mqtt-test-')
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        # Reserve an unused BMW endpoint without listening: no external calls.
        bmw_socket = socket.socket()
        bmw_socket.bind(('127.0.0.1', 0))
        self.addCleanup(bmw_socket.close)
        bmw_port = bmw_socket.getsockname()[1]
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            local_port = reserve.getsockname()[1]
        config = directory / 'mosquitto.conf'
        config.write_text(f'listener {local_port} 127.0.0.1\nallow_anonymous true\nlog_type all\n')
        broker_log = directory / 'broker.log'
        broker = self.start_process(['mosquitto', '-c', str(config), '-v'],
                                    os.environ.copy(), broker_log)

        def broker_ready():
            try:
                with socket.create_connection(('127.0.0.1', local_port), timeout=0.2):
                    return True
            except OSError:
                return False

        self.wait_for(broker_ready, [broker], [broker_log])
        payload = base64.urlsafe_b64encode(json.dumps(
            {'exp': int(time.time()) + 3600}).encode()).decode().rstrip('=')
        processes, logs, expected_ids = [broker], [broker_log], []
        for index, (client_id, prefix, override) in enumerate(zip(client_ids, prefixes, overrides)):
            state = directory / f'account-{index}'
            tokens = state / 'bmw-mqtt-bridge'
            tokens.mkdir(parents=True)
            settings = {'CLIENT_ID': client_id, 'GCID': f'test-account-{index}',
                        'BMW_HOST': '127.0.0.1', 'BMW_PORT': str(bmw_port),
                        'LOCAL_HOST': '127.0.0.1', 'LOCAL_PORT': str(local_port),
                        'LOCAL_USER': '', 'LOCAL_PASSWORD': '',
                        'STATUS_STABLE_DELAY': '0',
                        'LOCAL_PREFIX': prefix}
            if override is not None:
                settings['LOCAL_CLIENT_ID'] = override
            (tokens / '.env').write_text(''.join(f'{key}={value}\n' for key, value in settings.items()))
            (tokens / 'id_token.txt').write_text(f'e30.{payload}.c2ln')
            (tokens / 'refresh_token.txt').write_text('artificial-refresh-token')
            # Avoid inherited credentials or overrides from the developer's shell.
            environment = {key: value for key, value in os.environ.items()
                           if key not in settings and key != 'LOCAL_CLIENT_ID'}
            environment['XDG_STATE_HOME'] = str(state)
            log = directory / f'bridge-{index}.log'
            processes.append(self.start_process([str(self.binary)], environment, log))
            logs.append(log)
            expected_ids.append(override or 'bmw-bridge-' + client_id)

        def both_connected():
            text = broker_log.read_text()
            return all(f' as {client_id} (' in text for client_id in expected_ids)

        self.wait_for(both_connected, processes, logs)
        # Give duplicate-ID takeovers and automatic reconnects time to surface.
        deadline = time.monotonic() + 2.5
        while time.monotonic() < deadline:
            for process in processes:
                self.assertIsNone(process.poll(), '\n'.join(log.read_text() for log in logs))
            text = broker_log.read_text()
            self.assertNotIn('closing old connection', text, text)
            for client_id in expected_ids:
                self.assertNotIn(f'Client {client_id} disconnected', text, text)
            time.sleep(0.05)
        text = broker_log.read_text()
        for client_id in expected_ids:
            self.assertEqual(text.count(f' as {client_id} ('), 1, text)
            self.assertIn(f'Received PUBLISH from {client_id} ', text, text)

    def test_distinct_bmw_clients_with_default_prefix(self):
        self.check_clients(('bmw/', 'bmw/'), (None, None))

    def test_distinct_prefixes_do_not_change_client_ids(self):
        self.check_clients(('first/account/', 'second/account/'), (None, None))

    def test_empty_override_uses_default(self):
        self.check_clients(('bmw/', 'bmw/'), ('', ''))

    def test_explicit_overrides_are_used_unchanged(self):
        self.check_clients(('bmw/', 'bmw/'), ('custom-local-1', 'custom-local-2'),
                           client_ids=(CLIENT_IDS[0], CLIENT_IDS[0]))


if __name__ == '__main__':
    unittest.main()
