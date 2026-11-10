"""Resident E2EE through the production authenticated server and real workspaces."""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import unittest
import server_access as base
import workflows

PROBE = str(Path(sys.argv.pop(1)).resolve())


class SealedResident(unittest.TestCase):
    cmd = base.ServerAccess.cmd
    http = base.ServerAccess.http
    serving = base.ServerAccess.serving
    commit = base.ServerAccess.commit

    def setUp(self):
        base.ServerAccess.setUp(self)
        self.env = {k: v for k, v in self.env.items() if not k.upper().startswith('SYRNCK_')}
        self.env['SYRNCK_SEALED_KEY'] = '71' * 32
        self.wire = self.root / 'sender-wire'
        self.config['branches'] = [{'name': 'main', 'fast_forward': True, 'lock_paths': []}]
        for principal in self.config['principals']:
            principal['write_paths'] = [] if principal['role'] == 'reader' else ['sealed']
            principal['branches'] = [] if principal['role'] == 'reader' else ['main']
        self.access.write_text(json.dumps(self.config), encoding='utf-8')

    def arguments(self, *extra, store=None, directory=None, wire=None):
        return [workflows.CLI, '--store', str(store or self.local), 'agent', 'run', self.url,
                '--directory', str(directory or self.project), '--sealed', '--wire', str(wire or self.wire),
                '--interval-ms', '50', '--remote-poll-ms', '100', '--polling', *map(str, extra)]

    def once(self, *extra, actor='alice', expected=0, **paths):
        result = subprocess.run(self.arguments('--max-updates', '1', *extra, **paths), cwd=self.project,
                                env=dict(self.env, SYRNCK_TOKEN=self.tokens[actor]), capture_output=True, timeout=40)
        self.assertEqual(result.returncode, expected, (result.stdout, result.stderr))
        frame, = [json.loads(line) for line in result.stdout.splitlines()]
        return frame

    def refs(self, store=None):
        return json.loads(self.cmd('refs', store=store).stdout)

    def peer(self):
        directory = self.root / 'peer'
        directory.mkdir()
        store, wire = directory / '.syrnck', self.root / 'peer-wire'
        self.cmd('init', store=store)
        return dict(store=store, directory=directory, wire=wire)

    def test_bidirectional_rename_delete_merge_and_unchanged_poll(self):
        self.commit('Content/Private.uasset', b'PRIVATE-RESIDENT-BYTES' * 2048)
        self.commit('Source/code.cpp', b'one\ntwo\nthree\n')
        original = self.refs()['heads/main']['id']
        peer = self.peer()
        with self.serving(strict=True):
            first = self.once('--require-lease')
            self.assertEqual(first['state'], 'pushed')
            carrier = self.http('GET', '/v1/refs')['main']['id']
            self.assertNotEqual(carrier, original)
            received = self.once('--bidirectional', '--require-read-lease', **peer)
            self.assertEqual((received['state'], received['local_revision']), ('pulled', original))
            self.assertEqual((peer['directory'] / 'Content/Private.uasset').read_bytes(),
                             (self.project / 'Content/Private.uasset').read_bytes())
            self.assertEqual(self.once('--bidirectional', **peer)['state'], 'synced')
            wire_before = self.refs(peer['wire'])
            original_before = self.refs(peer['store'])
            idle = self.once('--bidirectional', **peer)
            self.assertEqual(idle['state'], 'synced')
            self.assertIsNone(idle['transfer'])
            self.assertEqual(self.refs(peer['wire']), wire_before)
            self.assertEqual(self.refs(peer['store']), original_before)
            # Both sides edit offline from the shared original. The native
            # merge must preserve a rename, a deletion and the other peer's add.
            (self.project / 'Content/Private.uasset').rename(self.project / 'Content/Renamed.uasset')
            (self.project / 'Source/code.cpp').unlink()
            pushed = self.once('--bidirectional')
            (peer['directory'] / 'peer.txt').write_bytes(b'peer offline change\n')
            merged = self.once('--bidirectional', '--rename-similarity', '80', **peer)
            self.assertTrue(merged['merged'])
            self.assertEqual(merged['state'], 'pushed')
            merge = json.loads(self.cmd('inspect', 'revision', merged['local_revision'], store=peer['store']).stdout)
            self.assertEqual(len(merge['parents']), 2)
            self.assertIn(pushed['local_revision'], merge['parents'])
            self.assertEqual(self.once('--bidirectional')['state'], 'pulled')
            self.assertTrue((self.project / 'Content/Renamed.uasset').exists())
            self.assertFalse((self.project / 'Source/code.cpp').exists())
            self.assertEqual((self.project / 'peer.txt').read_bytes(), b'peer offline change\n')
        for store in (self.local, peer['store'], self.wire, peer['wire']):
            self.cmd('gc', '--delete', store=store)
            self.cmd('fsck', store=store)

    def test_same_original_wire_rollback_and_deletion_are_blocked(self):
        self.commit('secret.txt', b'private')
        with self.serving(strict=True):
            self.once()
            first = self.http('GET', '/v1/refs')['main']['id']
            # Force a new valid carrier with the SAME private original by
            # preparing from scratch, then advancing public policy explicitly.
        self.config['branches'][0]['fast_forward'] = False
        self.access.write_text(json.dumps(self.config), encoding='utf-8')
        alternate = self.root / 'alternate-wire'
        prepared = json.loads(self.cmd('sealed', 'snapshot', 'heads/main', '--wire', alternate).stdout)
        self.assertNotEqual(prepared['snapshot'], first)
        # A same-original fork must not look synced, even though logical IDs match.
        with self.serving(strict=True):
            self.cmd('agent', 'push', self.url, 'sealed/prepared/' + prepared['snapshot'], 'main',
                     store=alternate, actor='admin')
            before = self.refs()
            blocked = self.once(expected=2)
            self.assertEqual(blocked['reason'], 'remote_history_rewritten')
            self.assertEqual(self.refs(), before)
        self.cmd('unpin', 'main', store=self.server, key=True)
        with self.serving(strict=True):
            blocked = self.once(expected=2)
            self.assertEqual(blocked['reason'], 'remote_deleted')
            self.assertEqual(self.http('GET', '/v1/refs'), {})

    def test_completed_wire_receipt_restores_missing_freshness_frontier(self):
        self.commit('secret.txt', b'private')
        with self.serving(strict=True):
            self.once()
            first = self.http('GET', '/v1/refs')['main']['id']
            accepted, = [key for name, key in self.refs().items() if name.startswith('sealed/checkpoints/')]
        # Simulate an older plaintext restore with no resident/crypto frontier;
        # the independently retained completed wire receipt still proves it.
        for name in self.refs():
            if name.startswith('sealed/checkpoints/') or '/sync/push/' in '/' + name:
                self.cmd('unpin', name)
        alternate = self.root / 'alternate-wire'
        prepared = json.loads(self.cmd('sealed', 'snapshot', 'heads/main', '--wire', alternate).stdout)
        self.config['branches'][0]['fast_forward'] = False
        self.access.write_text(json.dumps(self.config), encoding='utf-8')
        with self.serving(strict=True):
            self.cmd('agent', 'push', self.url, 'sealed/prepared/' + prepared['snapshot'], 'main',
                     store=alternate, actor='admin')
            self.assertEqual(self.once(expected=2)['reason'], 'remote_history_rewritten')
        checkpoint, = [key for name, key in self.refs().items()
                        if name.startswith('sealed/checkpoints/') and '/agent-candidates/' not in name]
        self.assertEqual(checkpoint, accepted)
        self.assertNotEqual(prepared['snapshot'], first)

    def test_offline_capture_rejoins_and_plain_mode_never_checks_out_carriers(self):
        self.commit('asset.bin', b'private initial')
        peer = self.peer()
        # No listening server: the new original still becomes durable locally.
        (self.project / 'offline.txt').write_bytes(b'offline retained edit')
        offline = self.once('--retries', '0', expected=1)
        self.assertEqual(offline['state'], 'unavailable')
        original = self.refs()['heads/main']['id']
        self.assertEqual(offline['local_revision'], original)
        with self.serving(strict=True):
            self.assertEqual(self.once()['state'], 'pushed')
            arguments = self.arguments('--bidirectional', '--max-updates', '1', **peer)
            arguments.remove('--sealed')
            at = arguments.index('--wire')
            del arguments[at:at+2]
            plain = subprocess.run(arguments, cwd=self.project,
                env=dict(self.env, SYRNCK_TOKEN=self.tokens['alice']), capture_output=True, timeout=30)
            self.assertEqual(plain.returncode, 1, plain.stderr)
            frame, = [json.loads(line) for line in plain.stdout.splitlines()]
            self.assertIn('requires agent --sealed', frame['message'])
            self.assertFalse((peer['directory'] / 'sealed').exists())
            self.assertNotIn('heads/main', self.refs(peer['store']))
            self.assertEqual(self.once('--bidirectional', **peer)['local_revision'], original)
            self.assertEqual((peer['directory'] / 'offline.txt').read_bytes(), b'offline retained edit')

    def test_private_root_mismatch_and_wire_location_fail_before_checkout(self):
        self.commit('asset.bin', b'private')
        peer = self.peer()
        with self.serving(strict=True):
            self.once()
            before = self.refs(peer['store'])
            blocked = self.once('--bidirectional', '--sealed-root', 'heads/other', expected=1, **peer)
            self.assertEqual(blocked['state'], 'unavailable')
            self.assertIn('configured private revision root', blocked['message'])
            self.assertEqual(self.refs(peer['store']), before)
            self.assertFalse((peer['directory'] / 'asset.bin').exists())
        nested = self.project / 'wire-cache'
        result = subprocess.run(self.arguments('--max-updates', '1', wire=nested), cwd=self.project,
            env=dict(self.env, SYRNCK_TOKEN=self.tokens['alice']), capture_output=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(nested.exists())

    def test_protected_master_is_loaded_once_and_signed_carriers_hide_originals(self):
        self.env['SYRNCK_SIGNING_SEED'] = '29' * 32
        identity = json.loads(self.cmd('native', 'identity', '--name', 'Alice', '--email',
                                       'alice@example.invalid').stdout)
        self.config['version'] = 2
        self.config['branches'][0]['require_signatures'] = True
        for principal in self.config['principals']:
            principal['signing_keys'] = [{field: identity[field] for field in ('public_key', 'name', 'email')}] \
                if principal['id'] == 'alice' else []
        self.access.write_text(json.dumps(self.config), encoding='utf-8')
        original = self.commit('Content/Hidden.uasset', b'CONFIDENTIAL-ASSET')
        envelope = self.root / 'resident-master.key'
        self.env['SYRNCK_SEALED_KEY_PASSWORD'] = 'Test protected master 密钥'
        self.cmd('sealed-key', 'protect', envelope)
        self.env.pop('SYRNCK_SEALED_KEY')
        self.env['SYRNCK_SEALED_KEY_FILE'] = str(envelope)
        lines = queue.Queue()
        with self.serving(strict=True):
            child = subprocess.Popen(self.arguments('--require-lease'), cwd=self.project,
                env=dict(self.env, SYRNCK_TOKEN=self.tokens['alice']), stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            def read():
                for line in child.stdout:
                    lines.put(json.loads(line))
            reader = threading.Thread(target=read, daemon=True)
            reader.start()
            try:
                self.assertEqual(lines.get(timeout=20)['state'], 'pushed')
                envelope.unlink()
                (self.project / 'Content/Hidden.uasset').write_bytes(b'CONFIDENTIAL-NEXT')
                second = lines.get(timeout=20)
                self.assertEqual(second['state'], 'pushed')
                self.assertNotEqual(second['local_revision'], original)
                carrier = self.http('GET', '/v1/refs')['main']['id']
                # Read all wire objects through the production HTTP protocol.
                import urllib.request
                todo, seen = [('revision', carrier)], set()
                while todo:
                    kind, identifier = todo.pop()
                    if (kind, identifier) in seen:
                        continue
                    seen.add((kind, identifier))
                    request = urllib.request.Request(self.url + f'/v1/objects/{kind}/{identifier}',
                        headers={'Authorization': 'Bearer ' + self.tokens['reader']})
                    with urllib.request.urlopen(request, timeout=10) as response:
                        data = response.read()
                    for private in (b'CONFIDENTIAL', b'Hidden.uasset', b'heads/main', original.encode(),
                                    second['local_revision'].encode()):
                        self.assertNotIn(private, data)
                    if kind != 'chunk':
                        value = json.loads(data)
                        if kind == 'revision':
                            self.assertEqual(value['signature']['public_key'], identity['public_key'])
                            todo.extend(('revision', parent) for parent in value['parents'])
                            todo.extend(('manifest', entry['manifest']) for entry in value['files'].values())
                        else:
                            todo.extend(('chunk', block['id']) for block in value['blocks'])
            finally:
                child.kill()
                reader.join(timeout=2)
                child.communicate(timeout=5)
        # A new process really must reload its configured, now missing key.
        result = subprocess.run(self.arguments('--max-updates', '1'), cwd=self.project,
            env=dict(self.env, SYRNCK_TOKEN=self.tokens['alice']), capture_output=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b'')

    def interrupt(self, phase):
        child = subprocess.Popen([PROBE, 'agent', str(self.local), str(self.wire), self.url, phase, 'plain'],
            cwd=self.project, env=dict(self.env, SYRNCK_TOKEN=self.tokens['alice']),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        lines = queue.Queue()
        reader = threading.Thread(target=lambda: lines.put(child.stdout.readline()), daemon=True)
        reader.start()
        try:
            line = lines.get(timeout=30)
            if not line:
                self.fail(child.communicate(timeout=5)[1].decode('utf-8', 'replace'))
            self.assertTrue(json.loads(line)['paused'])
            child.kill()
            child.communicate(timeout=5)
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=5)
            reader.join(timeout=2)
        sessions = json.loads(self.cmd('agent', 'sessions', store=self.wire).stdout)
        self.assertEqual(len(sessions), 1)
        return sessions[0]

    def killed(self, phase):
        before = self.refs()['heads/main']['id']
        with self.serving(strict=True):
            old = self.interrupt(phase)
            if phase != 'transfer':
                self.assertEqual(old['state'], 'publishing')
                self.assertEqual(self.http('GET', '/v1/refs')['main'], old['root'])
            # Advance the local HEAD before resuming the physical old session.
            newer = self.commit('newer.txt', b'new head after kill')
            resumed = self.once('--commits-only')
            self.assertEqual((resumed['state'], resumed['local_revision']), ('pushed', newer))
            completed = json.loads(self.cmd('agent', 'sessions', store=self.wire).stdout)
            self.assertEqual({row['state'] for row in completed}, {'complete'} if phase != 'transfer'
                             else {'transferring', 'complete'})
            self.assertIn(old['id'], {row['id'] for row in completed})
            self.assertEqual(self.once('--commits-only')['state'], 'synced')
        self.cmd('gc', '--delete')
        self.cmd('native', 'export', before, self.root / ('retained-' + phase))
        self.cmd('fsck')

    def test_interrupted_upload_reuses_the_exact_ciphertext_session(self):
        original = self.commit('asset.bin', b'private large asset' * 16384)
        with self.serving(strict=True):
            old = self.interrupt('transfer')
            self.assertEqual(old['state'], 'transferring')
            pins = {name: key for name, key in self.refs().items() if name.startswith('sealed/agent-candidates/')}
            self.assertEqual(list(pins.values()), [{'kind': 'revision', 'id': original}])
            self.cmd('gc', '--delete')
            resumed = self.once('--commits-only')
            self.assertEqual(resumed['state'], 'pushed')
            complete, = json.loads(self.cmd('agent', 'sessions', store=self.wire).stdout)
            self.assertEqual((complete['id'], complete['state']), (old['id'], 'complete'))
            self.assertEqual(self.http('GET', '/v1/refs')['main'], old['root'])

    def test_new_head_cannot_bypass_deleted_channel_after_proven_publication(self):
        self.commit('asset.bin', b'private old asset' * 4096)
        with self.serving(strict=True):
            old = self.interrupt('after-sealed-observation')
            self.assertEqual(old['state'], 'publishing')
        self.cmd('unpin', 'main', store=self.server, key=True)
        newer = self.commit('next.txt', b'new local version')
        with self.serving(strict=True):
            # The strict production server retains a durable lease receipt.
            # Reconcile that proven historical fact, then block the deletion.
            blocked = self.once('--commits-only', expected=2)
            self.assertEqual((blocked['reason'], blocked['local_revision']), ('remote_deleted', newer))
            self.assertEqual(self.http('GET', '/v1/refs'), {})
            completed, = json.loads(self.cmd('agent', 'sessions', store=self.wire).stdout)
            self.assertEqual((completed['id'], completed['state']), (old['id'], 'complete'))
        self.cmd('gc', '--delete')
        self.cmd('fsck')

    def test_kill_after_crypto_before_logical_observation(self):
        self.commit('asset.bin', b'private old asset' * 4096)
        self.killed('after-sealed-observation')

    def test_kill_after_logical_before_journal_completion(self):
        self.commit('asset.bin', b'private old asset' * 4096)
        self.killed('after-native-observation')


if __name__ == '__main__':
    unittest.main()
