"""Rename/history CLI compared with Git's exact rename and follow behavior."""
import json
import subprocess
import unittest
import native_workflows as base


class NativeFileHistory(unittest.TestCase):
    setUp=base.NativeWorkflows.setUp
    tearDown=base.NativeWorkflows.tearDown
    call=base.NativeWorkflows.call
    cmd=base.NativeWorkflows.cmd
    snapshot=base.NativeWorkflows.snapshot

    def git(self,*args):
        run=subprocess.run(["git","-c","safe.directory="+self.root.as_posix(),*map(str,args)],
                           cwd=self.root,env=self.env,capture_output=True,timeout=15)
        self.assertEqual(run.returncode,0,run.stderr.decode("utf-8","replace"))
        return run.stdout

    def test_exact_rename_and_history_match_git_with_unicode_names(self):
        self.git("init")
        self.git("config","user.name","History test")
        self.git("config","user.email","history@example.invalid")
        old="old Édit.cpp";new="new édit.cpp"
        (self.root/old).write_bytes(b"first\nsecond\n")
        first=self.cmd("native","commit","-m","first").decode().strip()
        self.git("add","--",old);self.git("commit","-m","first")
        git_first=self.git("rev-parse","HEAD").decode().strip()
        (self.root/old).rename(self.root/new)
        second=self.cmd("native","commit","-m","renamed").decode().strip()
        self.git("add","-A","--",old,new);self.git("commit","-m","renamed")
        git_second=self.git("rev-parse","HEAD").decode().strip()
        before=self.snapshot()
        result=json.loads(self.call("native","diff",first,second,"--renames","--progress-json").stdout)
        rename=result["changes"][0]
        self.assertEqual((rename["status"],rename["old_path"],rename["path"],rename["similarity"]),("renamed",old,new,100))
        oracle=self.git("diff","--find-renames=100%","--name-status","-z",git_first,git_second).split(b"\0")
        self.assertEqual(oracle,[b"R100",old.encode(),new.encode(),b""])
        self.assertEqual(self.snapshot(),before)
        (self.root/new).write_bytes(b"modified\nsecond\n")
        third=self.cmd("native","commit","-m","modified").decode().strip()
        self.git("add","--",new);self.git("commit","-m","modified")
        before=self.snapshot()
        history=json.loads(self.call("--lang","fr","native","log-file",third,new).stdout)
        self.assertEqual([row["revision"] for row in history["entries"]],[third,second,first])
        oracle=self.git("log","--follow","--format=%s","--",new).decode().splitlines()
        self.assertEqual([row["message"] for row in history["entries"]],oracle)
        page=json.loads(self.call("native","log-file",third,new,"--limit",1).stdout)
        rest=json.loads(self.call("native","log-file",page["next_revision"],page["next_path"]).stdout)
        self.assertEqual(len(rest["entries"]),2)
        self.assertEqual(self.snapshot(),before)
        self.call("native","log-file",third,new,"--limit",0,ok=False)


if __name__=="__main__":unittest.main()
