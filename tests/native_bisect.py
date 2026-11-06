"""Persistent native bisect CLI across fresh processes, skipped builds and GC."""
import json
import unittest
import native_workflows as base


class NativeBisect(unittest.TestCase):
    setUp=base.NativeWorkflows.setUp
    tearDown=base.NativeWorkflows.tearDown
    call=base.NativeWorkflows.call
    cmd=base.NativeWorkflows.cmd

    def test_persistent_search_no_checkout_state_fencing_and_archive(self):
        path=self.root/"code.cpp"
        revisions=[]
        for number in range(12):
            path.write_text(f"int revision = {number};\n",encoding="utf-8")
            revisions.append(self.cmd("native","commit","-m",str(number)).decode().strip())
        refs=json.loads(self.cmd("refs"))
        working=path.read_bytes()
        state=json.loads(self.call("native","bisect-start",revisions[0],revisions[-1],"--progress-json").stdout)
        first_state=state["state"]
        while state["status"]=="testing":
            candidate=state["next_revision"]
            result="bad" if revisions.index(candidate)>=5 else "good"
            previous=state["state"]
            state=json.loads(self.call("--lang","fr","native","bisect-"+result,candidate,"--expect-state",previous).stdout)
            rejected=self.call("--lang","fr","native","bisect-"+result,candidate,"--expect-state",previous,ok=False)
            self.assertIn("état",rejected.stderr.decode("utf-8"))
            self.call("gc","--delete")
        self.assertEqual(state["first_bad"],revisions[5])
        self.assertEqual(path.read_bytes(),working)
        self.assertEqual(json.loads(self.cmd("refs"))["heads/main"],refs["heads/main"])
        ended=json.loads(self.call("native","bisect-reset","--expect-state",state["state"]).stdout)
        self.assertFalse(ended["active"])
        self.assertEqual(ended["archive"],"archive/bisect/"+revisions[-1])
        self.assertFalse(json.loads(self.call("native","bisect-status").stdout)["active"])
        self.call("gc","--delete")
        archives=json.loads(self.call("native","archives","--kind","bisect").stdout)["archives"]
        self.assertEqual(len(archives),1)
        restarted=json.loads(self.call("native","bisect-start",revisions[0],revisions[-1]).stdout)
        self.assertNotEqual(restarted["state"],first_state)
        self.call("native","bisect-reset","--expect-state",first_state,ok=False)

    def test_skipped_candidate_stays_ambiguous(self):
        revisions=[]
        for number in range(3):
            (self.root/"asset.bin").write_bytes(bytes([number]))
            revisions.append(self.cmd("native","commit","-m",str(number)).decode().strip())
        state=json.loads(self.call("native","bisect-start",revisions[0],revisions[2]).stdout)
        self.assertEqual(state["next_revision"],revisions[1])
        self.call("native","bisect-skip",revisions[1],ok=False)
        state=json.loads(self.call("native","bisect-skip",revisions[1],"--expect-state",state["state"]).stdout)
        self.assertEqual(state["status"],"ambiguous")
        self.assertIsNone(state["first_bad"])
        self.assertEqual(set(state["possible_first_bad"]),set(revisions[1:]))
        self.call("native","bisect-good",revisions[1],"--expect-state",state["state"],ok=False)


if __name__=="__main__":
    unittest.main()
