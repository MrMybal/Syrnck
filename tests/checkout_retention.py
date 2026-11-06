"""Backup-fenced checkout retirement and historical receipt independence."""
import json
import unittest
import workflows as base


class CheckoutRetention(unittest.TestCase):
    setUp = base.Workflows.setUp
    tearDown = base.Workflows.tearDown
    cmd = base.Workflows.cmd

    def test_atomic_group_backup_restore_gc_and_languages(self):
        asset = self.root / 'asset'
        asset.write_bytes(b'original')
        head = self.cmd('native', 'commit', '-m', 'base').decode().strip()
        asset.write_bytes(b'dirty')
        result = json.loads(self.cmd('native', 'reset', 'HEAD', '--hard'))
        transaction = result['id']
        backup = self.root / 'backup'
        self.cmd('backup', 'create', backup)
        rows = json.loads(self.cmd('native', 'checkout-receipts', '--verify'))
        self.assertEqual([r['id'] for r in rows['transactions']], [transaction])
        self.assertFalse(rows['transactions'][0]['retained_only'])
        plan = json.loads(self.cmd('backup', 'checkout-retire-plan', transaction, '--from', backup,
                                  '--include-source-archives'))
        french = json.loads(self.cmd('--lang', 'fr', 'backup', 'checkout-retire-plan', transaction,
                                    '--from', backup, '--include-source-archives'))
        self.assertEqual(plan, french)
        self.assertEqual(len(plan['references']), 3)
        self.cmd('backup', 'checkout-retire', transaction, '--from', backup, ok=False)
        self.cmd('pin', 'other', 'revision', head)
        self.cmd('backup', 'checkout-retire', transaction, '--from', backup, '--include-source-archives',
                 '--expect-plan', plan['plan'], ok=False)
        plan = json.loads(self.cmd('backup', 'checkout-retire-plan', transaction, '--from', backup,
                                  '--include-source-archives'))
        done = json.loads(self.cmd('backup', 'checkout-retire', transaction, '--from', backup,
                                  '--include-source-archives', '--expect-plan', plan['plan']))
        self.assertTrue(done['applied'])
        refs = json.loads(self.cmd('refs'))
        for name in ('checkouts/receipts/', 'checkouts/completed/', 'archive/checkout/'):
            self.assertNotIn(name + transaction, refs)
        self.assertEqual(refs['heads/main']['id'], head)
        self.assertEqual(asset.read_bytes(), b'original')
        self.cmd('gc', '--delete')
        export = self.store / 'exported'
        self.cmd('native', 'export', result['source'], export, store=backup)
        self.assertEqual((export / 'asset').read_bytes(), b'dirty')
        self.cmd('backup', 'restore', self.store / 'restored', store=backup)
        restored = self.store / 'restored'
        rows = json.loads(self.cmd('native', 'checkout-receipts', store=restored))
        self.assertTrue(rows['transactions'][0]['retained_only'])
        plan = json.loads(self.cmd('backup', 'checkout-retire-plan', transaction, '--from', backup, store=restored))
        self.cmd('backup', 'checkout-retire', transaction, '--from', backup, '--expect-plan', plan['plan'], store=restored)
        self.assertFalse(json.loads(self.cmd('native', 'checkout-receipts', store=restored))['transactions'])

    def test_old_receipts_survive_retired_source_and_old_head_selection(self):
        asset = self.root / 'asset'
        asset.write_bytes(b'base')
        self.cmd('native', 'commit', '-m', 'base')
        self.cmd('native', 'branch', 'other', 'HEAD')
        self.cmd('native', 'switch', 'other')
        asset.write_bytes(b'working')
        reset = json.loads(self.cmd('native', 'reset', 'HEAD', '--hard'))
        self.cmd('native', 'switch', 'main')
        backup = self.root / 'backup'
        self.cmd('backup', 'create', backup)
        name = 'archive/checkout/' + reset['id']
        plan = json.loads(self.cmd('backup', 'retire-plan', name, '--from', backup))
        self.cmd('backup', 'retire', name, '--from', backup, '--expect-plan', plan['plan'])
        self.cmd('gc', '--delete')
        receipt = json.loads(self.cmd('native', 'checkout-status', '--id', reset['id']))
        self.assertFalse(receipt['pending'])
        self.assertEqual(receipt['source'], reset['source'])
        self.assertTrue(any(r['id'] == reset['id'] for r in json.loads(self.cmd('native', 'checkout-receipts'))['transactions']))
        # The immutable receipt is readable even after its explicitly retired source is collected.
        self.cmd('inspect', 'revision', reset['source'], ok=False)
        asset.write_bytes(b'stashed')
        saved = json.loads(self.cmd('native', 'stash', 'push'))['stash']['id']
        plan = json.loads(self.cmd('native', 'stash', 'apply-plan', saved))
        applied = json.loads(self.cmd('native', 'stash', 'pop', saved, '--expect-plan', plan['plan']))
        self.cmd('native', 'reset', 'HEAD', '--hard')
        self.cmd('native', 'switch', 'other')
        self.cmd('gc', '--delete')
        self.assertFalse(json.loads(self.cmd('native', 'checkout-status', '--id', applied['id']))['pending'])


if __name__ == '__main__':
    unittest.main()
