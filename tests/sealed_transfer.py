"""Real authenticated ciphertext transfers and original-history/checkpoint guards."""
import json
import os
from pathlib import Path
import subprocess
import sys
import queue
import threading
import unittest
import urllib.request
import server_access as base
import workflows
PROBE = str(Path(sys.argv.pop(1)).resolve())


class SealedTransfers(unittest.TestCase):
    cmd = base.ServerAccess.cmd
    http = base.ServerAccess.http
    serving = base.ServerAccess.serving
    commit = base.ServerAccess.commit

    def setUp(self):
        base.ServerAccess.setUp(self)
        self.env['SYRNCK_SEALED_KEY'] = '71' * 32
        self.wire = self.root / 'sender-wire'
        self.clone = self.root / 'clone'
        self.clone_wire = self.root / 'receiver-wire'
        self.cmd('init', store=self.clone)
        self.config['branches'] = [{'name': 'main', 'fast_forward': True, 'lock_paths': []}]
        for principal in self.config['principals']:
            principal['write_paths'] = [] if principal['role'] == 'reader' else ['sealed']
            principal['branches'] = [] if principal['role'] == 'reader' else ['main']
        self.access.write_text(json.dumps(self.config), encoding='utf-8')

    def sealed(self, action, *args, clone=False, ok=True, **kwargs):
        result = self.cmd('sealed', action, *args, '--wire', self.clone_wire if clone else self.wire,
                          store=self.clone if clone else self.local, ok=ok, **kwargs)
        return json.loads(result.stdout) if ok else result

    def refs(self, store=None):
        return json.loads(self.cmd('refs', store=store).stdout)

    def checkpoint(self, store=None):
        refs = self.refs(store)
        return {name: key for name, key in refs.items() if name.startswith('sealed/checkpoints/')}

    def test_roundtrip_delta_signed_carriers_and_retention(self):
        self.env['SYRNCK_SIGNING_SEED'] = '29' * 32
        profile = json.loads(self.cmd('native', 'identity', '--name', 'Alice', '--email', 'alice@example.invalid').stdout)
        self.config['version'] = 2
        for principal in self.config['principals']:
            principal['signing_keys'] = [{field: profile[field] for field in ('public_key', 'name', 'email')}] if principal['id'] == 'alice' else []
        self.config['branches'][0]['require_signatures'] = True
        self.access.write_text(json.dumps(self.config), encoding='utf-8')
        original = self.commit('Content/PrivateAsset.uasset', b'PRIVATE-ASSET-PAYLOAD')
        self.commit('Source/PrivateCode.cpp', b'// PRIVATE-CODE-PAYLOAD\n')
        original = self.refs()['heads/main']['id']
        with self.serving(strict=True):
            first = self.sealed('push', self.url, 'heads/main', actor='alice', **{'ok': True})
            self.assertEqual(first['state'], 'pushed')
            self.assertEqual(self.http('GET', '/v1/refs')['main']['id'], first['snapshot'])
            fetched = self.sealed('fetch', self.url, clone=True, actor='reader', **{'ok': True})
            self.assertEqual(fetched['roots']['heads/main']['id'], original)
            self.assertNotIn('heads/main', self.refs(self.clone))
            self.assertTrue(self.checkpoint(self.clone))
            # Traverse every wire object through the actual server, including
            # decoded metadata. The server's at-rest key cannot reveal originals.
            pending = [('revision', first['snapshot'])]
            seen = set()
            while pending:
                kind, identifier = pending.pop()
                if (kind, identifier) in seen:
                    continue
                seen.add((kind, identifier))
                req = urllib.request.Request(self.url + f'/v1/objects/{kind}/{identifier}',
                    headers={'Authorization': 'Bearer ' + self.tokens['reader']})
                with urllib.request.urlopen(req, timeout=10) as response:
                    data = response.read()
                for private in (b'PRIVATE-ASSET-PAYLOAD', b'PrivateAsset', b'PRIVATE-CODE-PAYLOAD',
                                b'PrivateCode', original.encode(), b'heads/main'):
                    self.assertNotIn(private, data)
                if kind != 'chunk':
                    value = json.loads(data)
                    if kind == 'revision':
                        self.assertEqual(value['signature']['public_key'], profile['public_key'])
                        pending.extend(('revision', parent) for parent in value['parents'])
                        pending.extend(('manifest', entry['manifest']) for entry in value['files'].values())
                    else:
                        pending.extend(('chunk', block['id']) for block in value['blocks'])
            self.assertEqual(len(seen), first['transfer']['transferred_objects'])
            self.assertTrue(self.checkpoint())
            second_original = self.commit('Content/PrivateAsset.uasset', b'PRIVATE-ASSET-UPDATED')
            second = self.sealed('push', self.url, 'heads/main', actor='alice')
            self.assertEqual(second['sealed_objects'], 3)
            self.assertGreater(second['reused_objects'], 0)
            checkpoint = self.checkpoint()
            self.sealed('resume', self.url, first['transfer']['session'], actor='alice')
            self.assertEqual(self.checkpoint(), checkpoint, 'historical publication receipt rewound a newer checkpoint')
            fetched = self.sealed('fetch', self.url, clone=True, actor='reader', **{'ok': True})
            self.assertEqual(fetched['roots']['heads/main']['id'], second_original)
            unchanged = self.sealed('push', self.url, 'heads/main', actor='alice')
            self.assertEqual(unchanged['snapshot'], second['snapshot'])
            self.assertEqual(unchanged['transfer']['transferred_objects'], 0)
        self.cmd('gc', '--delete', store=self.clone)
        self.cmd('native', 'export', original, self.root / 'old-restored', store=self.clone)
        self.assertEqual((self.root / 'old-restored/Content/PrivateAsset.uasset').read_bytes(), b'PRIVATE-ASSET-PAYLOAD')
        self.cmd('native', 'export', second_original, self.root / 'new-restored', store=self.clone)
        self.assertEqual((self.root / 'new-restored/Content/PrivateAsset.uasset').read_bytes(), b'PRIVATE-ASSET-UPDATED')
        self.cmd('pack', '--compact', store=self.clone_wire)
        self.cmd('gc', '--delete', store=self.clone_wire)
        self.cmd('fsck', store=self.clone_wire)
        self.cmd('gc', '--delete', store=self.server, key=True)
        self.cmd('fsck', store=self.server, key=True)

    def test_server_rollback_and_missing_channel_are_refused(self):
        self.commit('Content/Secret.uasset', b'first')
        with self.serving(strict=True):
            first = self.sealed('push', self.url, 'heads/main', actor='alice')
            self.sealed('fetch', self.url, clone=True, actor='reader')
            self.commit('Content/Secret.uasset', b'second')
            second = self.sealed('push', self.url, 'heads/main', actor='alice')
            self.sealed('fetch', self.url, clone=True, actor='reader')
        accepted = self.refs(self.clone)
        # A hostile/restored server can still expose a cryptographically valid
        # old root. Neither fetch nor a subsequent push may accept the rollback.
        self.cmd('pin', 'main', 'revision', first['snapshot'], store=self.server, key=True)
        with self.serving(strict=True):
            denied = self.sealed('fetch', self.url, clone=True, actor='reader', ok=False)
            self.assertIn(b'rolls back or forks', denied.stderr)
            self.assertEqual(self.refs(self.clone), accepted)
            denied = self.sealed('push', self.url, 'heads/main', actor='alice', ok=False)
            self.assertIn(b'rolls back or forks', denied.stderr)
        self.cmd('pin', 'main', 'revision', second['snapshot'], store=self.server, key=True)
        self.cmd('unpin', 'main', store=self.server, key=True)
        with self.serving(strict=True):
            denied = self.sealed('push', self.url, 'heads/main', actor='alice', ok=False)
            self.assertIn(b'disappeared', denied.stderr)
            self.assertEqual(self.http('GET', '/v1/refs'), {})

    def test_original_rewrite_requires_explicit_flag(self):
        first = self.commit('Content/Asset.uasset', b'first')
        with self.serving(strict=True):
            self.sealed('push', self.url, 'heads/main', actor='alice')
            second = self.commit('Content/Asset.uasset', b'second')
            accepted = self.sealed('push', self.url, 'heads/main', actor='alice')
            self.cmd('pin', 'heads/main', 'revision', first)
            denied = self.sealed('push', self.url, 'heads/main', actor='alice', ok=False)
            self.assertIn(b'original history diverged', denied.stderr)
            self.assertEqual(self.http('GET', '/v1/refs')['main']['id'], accepted['snapshot'])
            rewritten = self.sealed('push', self.url, 'heads/main', '--allow-rewrite', actor='alice')
            self.assertEqual(rewritten['roots']['heads/main']['id'], first)
            # Rewritten originals remain retained through older sealed snapshots.
            self.sealed('fetch', self.url, clone=True, actor='reader')
            self.sealed('open', accepted['snapshot'], '--checkpoint', 'sealed/checkpoints/historical', clone=True)
            self.cmd('gc', '--delete', store=self.clone)
            self.cmd('native', 'export', second, self.root / 'retained', store=self.clone)

    def test_local_commands_key_files_and_invalid_locations(self):
        revision = self.commit('Content/Asset.uasset', b'private asset')
        self.cmd('native', 'checkout', revision, '--directory', self.project)
        self.env['SYRNCK_SEALED_KEY_PASSWORD'] = 'Test password 密钥'
        key = self.root / 'sealed.key'
        self.cmd('sealed-key', 'protect', key)
        self.env.pop('SYRNCK_SEALED_KEY')
        self.env['SYRNCK_SEALED_KEY_FILE'] = str(key)
        self.cmd('key', 'inspect', key, ok=False)
        self.cmd('signing-key', 'inspect', key, ok=False)
        verified = json.loads(self.cmd('sealed-key', 'verify', key).stdout)
        self.assertTrue(verified['authenticated'])
        sealed = self.sealed('snapshot', 'heads/main')
        inspected = self.sealed('inspect', sealed['snapshot'])
        self.assertEqual(inspected['roots']['heads/main']['id'], revision)
        self.sealed('open', sealed['snapshot'], '--checkpoint', 'sealed/checkpoints/manual', clone=True, ok=False)
        # Local restore reads the sender's wire store explicitly.
        self.cmd('sealed', 'open', sealed['snapshot'], '--wire', self.wire, '--checkpoint', 'sealed/checkpoints/manual', store=self.clone)
        self.env['SYRNCK_SEALED_KEY'] = '71' * 32
        self.sealed('inspect', sealed['snapshot'], ok=False)
        self.env.pop('SYRNCK_SEALED_KEY')
        self.env['SYRNCK_SEALED_KEY_PASSWORD'] = 'wrong'
        before = self.refs(self.clone)
        self.cmd('sealed', 'open', sealed['snapshot'], '--wire', self.wire, '--checkpoint', 'sealed/checkpoints/manual', store=self.clone, ok=False)
        self.assertEqual(self.refs(self.clone), before)
        self.env['SYRNCK_SEALED_KEY_PASSWORD'] = 'Test password 密钥'
        for path in (self.local / 'wire', self.project / 'wire-cache'):
            with self.subTest(wire=str(path)):
                self.cmd('sealed', 'snapshot', 'heads/main', '--wire', path, ok=False)
                self.assertFalse(path.exists(), 'invalid location created a wire store in the plaintext project')
        self.env['SYRNCK_NEW_SEALED_KEY_PASSWORD'] = 'New test password'
        rotated = self.root / 'rewrapped.key'
        self.cmd('sealed-key', 'rewrap', key, rotated)
        self.env['SYRNCK_SEALED_KEY_FILE'] = str(rotated)
        self.env['SYRNCK_SEALED_KEY_PASSWORD'] = 'New test password'
        after = json.loads(self.cmd('sealed-key', 'verify', rotated).stdout)
        self.assertEqual(after['realm'], verified['realm'])
        self.sealed('inspect', sealed['snapshot'])

    def killed_case(self, operation, phase):
        original = self.commit('Content/Retained.uasset', b'PRIVATE-KILL-PAYLOAD' * 8192)
        with self.serving(strict=True):
            if operation == 'fetch':
                self.sealed('push', self.url, 'heads/main', actor='alice')
            plain = self.local if operation == 'push' else self.clone
            wire = self.wire if operation == 'push' else self.clone_wire
            before = self.refs(plain)
            process = subprocess.Popen([PROBE, operation, str(plain), str(wire), self.url, phase, 'plain'],
                cwd=self.project, env=dict(self.env, SYRNCK_TOKEN=self.tokens['alice']),
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            lines = queue.Queue()
            reader = threading.Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True)
            reader.start()
            try:
                line = lines.get(timeout=30)
                if not line:
                    self.fail(process.communicate(timeout=5)[1].decode('utf-8', 'replace'))
                self.assertTrue(json.loads(line)['paused'])
                process.kill()
                process.communicate(timeout=5)
                reader.join(timeout=2)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)
            # The caller was killed, not cancelled or allowed to exit gracefully.
            self.assertNotEqual(process.returncode, 0)
            current = self.refs(plain)
            if phase != 'after-checkpoint':
                self.assertEqual(current, before)
            else:
                self.assertTrue(self.checkpoint(plain))
            sessions = json.loads(self.cmd('agent', 'sessions', store=wire).stdout)
            pending = [session for session in sessions if session['direction'] == ('push' if operation == 'push' else 'pull')]
            self.assertEqual(len(pending), 1, pending)
            session = pending[0]
            clone = operation == 'fetch'
            if operation == 'push':
                denied = self.sealed('push', self.url, 'heads/main', actor='alice', ok=False)
                self.assertIn(b'unfinished sealed publication', denied.stderr)
            resumed = self.sealed('resume', self.url, session['id'], clone=clone, actor='alice')
            self.assertEqual(resumed['roots']['heads/main']['id'], original)
            self.assertTrue(self.checkpoint(plain))
            if phase != 'transfer':
                self.assertEqual(resumed['transfer']['transferred_objects'], 0)
            self.sealed('resume', self.url, session['id'], clone=clone, actor='alice')
            self.assertEqual(self.http('GET', '/v1/refs')['main']['id'], resumed['snapshot'])
        self.cmd('gc', '--delete', store=plain)
        self.cmd('fsck', store=plain)
        self.cmd('native', 'export', original, self.root / 'after-kill', store=plain)
        self.assertEqual((self.root / 'after-kill/Content/Retained.uasset').read_bytes(), b'PRIVATE-KILL-PAYLOAD' * 8192)

    def test_killed_upload_is_resumed(self):
        self.killed_case('push', 'transfer')

    def test_killed_publication_before_checkpoint_is_reconciled(self):
        self.killed_case('push', 'before-checkpoint')

    def test_killed_publication_after_checkpoint_is_idempotent(self):
        self.killed_case('push', 'after-checkpoint')

    def test_killed_download_is_resumed(self):
        self.killed_case('fetch', 'transfer')

    def test_killed_download_before_checkpoint_is_reconciled(self):
        self.killed_case('fetch', 'before-checkpoint')

    def test_killed_download_after_checkpoint_is_idempotent(self):
        self.killed_case('fetch', 'after-checkpoint')


if __name__ == '__main__':
    unittest.main()
