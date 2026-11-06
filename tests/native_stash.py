"""Stash commands preserve branch, working files, index, retention and language boundaries."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest
import native_workflows as base


class Stash(unittest.TestCase):
    setUp = base.NativeWorkflows.setUp
    tearDown = base.NativeWorkflows.tearDown
    cmd = base.NativeWorkflows.cmd
    call = base.NativeWorkflows.call
    snapshot = base.NativeWorkflows.snapshot
    commit = base.NativeWorkflows.commit

    def report(self, *args):
        return json.loads(self.call('native', 'stash', *args).stdout)

    def fixture(self):
        for name, content in {'a': b'base', 'deleted': b'remove', 'shape': b'flat'}.items():
            (self.root / name).write_bytes(content)
        head = self.commit('base')
        (self.root / 'a').write_bytes(b'staged')
        (self.root / 'added').write_bytes(b'staged new')
        index = self.call('native', 'add', 'a', 'added').stdout.decode().strip()
        (self.root / 'a').write_bytes(b'working')
        (self.root / 'added').write_bytes(b'working new')
        (self.root / 'deleted').unlink()
        (self.root / 'shape').unlink()
        (self.root / 'shape').mkdir()
        (self.root / 'shape/child').write_bytes(b'nested')
        (self.root / 'outside').write_bytes(b'untracked')
        return head, index

    def test_push_pop_index_language_and_receipt(self):
        head, index = self.fixture()
        saved = self.report('push', '--include-untracked', '-m', 'Travail édité \\ source')['stash']
        self.assertEqual(self.report('show', saved['id'], '--verify')['message'], 'Travail édité \\ source')
        self.assertEqual((self.root / 'a').read_bytes(), b'base')
        self.assertFalse((self.root / 'outside').exists())
        before = self.snapshot()
        plan = self.report('apply-plan', saved['id'], '--index', '--verify')
        self.assertTrue(plan['ready'])
        self.assertEqual(self.snapshot(), before)
        fr = json.loads(self.call('--lang', 'fr', 'native', 'stash', 'apply-plan', saved['id'], '--index', '--verify').stdout)
        self.assertEqual(plan, fr)
        self.call('native', 'stash', 'pop', saved['id'], '--index', ok=False)
        self.assertEqual(self.snapshot(), before)
        result = self.report('pop', saved['id'], '--index', '--expect-plan', plan['plan'])
        self.assertEqual(result['version'], 6)
        refs = json.loads(self.cmd('refs'))
        self.assertEqual(refs['heads/main']['id'], head)
        self.assertEqual((self.root / 'a').read_bytes(), b'working')
        self.assertEqual((self.root / 'added').read_bytes(), b'working new')
        self.assertEqual((self.root / 'outside').read_bytes(), b'untracked')
        self.assertEqual((self.root / 'shape/child').read_bytes(), b'nested')
        self.assertFalse((self.root / 'deleted').exists())
        self.assertEqual(self.report('list')['total'], 0)
        self.call('gc', '--delete')
        self.call('native', 'export', refs['staging/main']['id'], self.store / 'restored-index')
        self.assertEqual((self.store / 'restored-index/a').read_bytes(), b'staged')
        self.assertEqual((self.store / 'restored-index/added').read_bytes(), b'staged new')
        self.assertEqual(self.report('archive', saved['id'], '--verify')['index'], index)
        self.assertFalse(json.loads(self.call('native', 'checkout-status', '--id', result['id']).stdout)['pending'])

    def test_stale_plan_dirty_obstruction_and_archived_restore(self):
        (self.root / 'a').write_bytes(b'base')
        self.commit('base')
        (self.root / 'a').write_bytes(b'saved')
        (self.root / 'outside').write_bytes(b'untracked')
        saved = self.report('push', '--include-untracked')['stash']
        plan = self.report('apply-plan', saved['id'])
        (self.root / 'a').write_bytes(b'dirty')
        before = self.snapshot()
        self.call('native', 'stash', 'apply', saved['id'], '--expect-plan', plan['plan'], ok=False)
        self.assertEqual(self.snapshot(), before)
        (self.root / 'a').write_bytes(b'base')
        (self.root / 'outside').write_bytes(b'different untracked')
        result = subprocess.run([base.base.CLI, '--store', str(self.store), 'native', 'stash',
                                  'apply-plan', saved['id']], cwd=self.root, env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 1, result.stderr.decode())
        self.assertFalse(json.loads(result.stdout)['ready'])
        self.assertEqual((self.root / 'outside').read_bytes(), b'different untracked')
        (self.root / 'outside').unlink()
        self.report('drop', saved['id'], '--expect-record', saved['record'])
        before = self.snapshot()
        self.call('native', 'stash', 'apply', saved['id'], '--expect-plan', plan['plan'], ok=False)
        self.assertEqual(self.snapshot(), before)
        self.report('restore', saved['id'])
        fresh = self.report('apply-plan', saved['id'])
        self.assertEqual(fresh['plan'], plan['plan'])
        self.report('apply', saved['id'], '--expect-plan', fresh['plan'])
        self.assertEqual((self.root / 'a').read_bytes(), b'saved')

    def test_archive_generation_and_french_error(self):
        (self.root / 'a').write_bytes(b'base')
        self.commit('base')
        ids = []
        for content in (b'one', b'two', b'three'):
            (self.root / 'a').write_bytes(content)
            ids.append(self.report('push')['stash'])
        page = self.report('list', '--limit', '2')
        self.assertEqual([e['id'] for e in page['stashes']], [ids[2]['id'], ids[1]['id']])
        self.assertEqual(self.report('list', '--before', page['next_before'])['stashes'][0]['id'], ids[0]['id'])
        self.call('native', 'stash', 'drop', ids[1]['id'], '--expect-record', 'a'*64, ok=False)
        self.report('drop', ids[1]['id'], '--expect-record', ids[1]['record'])
        self.call('gc', '--delete')
        self.report('archive', ids[1]['id'], '--verify')
        self.report('restore', ids[1]['id'])
        self.assertEqual(self.report('list')['total'], 3)
        error = self.call('--lang', 'fr', 'native', 'stash', 'drop', ids[1]['id'], ok=False)
        self.assertIn('exige --expect-record', error.stderr.decode())
        self.assertIsNone(self.report('push')['stash'])

    @unittest.skipUnless(shutil.which('git'), 'Git oracle unavailable')
    def test_git_stash_index_oracle(self):
        git_root = self.store / 'git-oracle'
        git_root.mkdir()
        def git(*args):
            result = subprocess.run(['git', '-c', 'core.autocrlf=false', '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                                      *args], cwd=git_root, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', 'replace'))
            return result.stdout
        git('init')
        for directory in (self.root, git_root):
            (directory / 'a').write_bytes(b'base\n')
            (directory / 'deleted').write_bytes(b'deleted\n')
        self.commit('base')
        git('add', '.')
        git('commit', '-m', 'base')
        for directory in (self.root, git_root):
            (directory / 'a').write_bytes(b'staged\n')
        self.call('native', 'add', 'a')
        git('add', 'a')
        for directory in (self.root, git_root):
            (directory / 'a').write_bytes(b'working\n')
            (directory / 'deleted').unlink()
            (directory / 'outside').write_bytes(b'untracked\n')
        saved = self.report('push', '--include-untracked')['stash']
        git('stash', 'push', '--include-untracked')
        plan = self.report('apply-plan', saved['id'], '--index')
        self.report('pop', saved['id'], '--index', '--expect-plan', plan['plan'])
        git('stash', 'pop', '--index')
        for name in ('a', 'deleted', 'outside'):
            self.assertEqual((self.root / name).exists(), (git_root / name).exists())
            if (self.root / name).exists():
                self.assertEqual((self.root / name).read_bytes(), (git_root / name).read_bytes())
        refs = json.loads(self.cmd('refs'))
        self.call('native', 'export', refs['staging/main']['id'], self.store / 'staged-oracle')
        self.assertEqual((self.store / 'staged-oracle/a').read_bytes(), git('show', ':a'))


if __name__ == '__main__':
    unittest.main()
