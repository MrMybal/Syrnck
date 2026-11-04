"""Production filter-process framing, recovery, installation and Git reuse."""
import json
import unittest
import git_catalog as base


def packet(data):
    return f"{len(data) + 4:04x}".encode() + data


def lines(*values):
    return b"".join(packet(value) for value in values) + b"0000"


def handshake(capabilities=(b"clean", b"smudge", b"delay")):
    return lines(b"git-filter-client\n", b"version=2\n") + lines(
        *(b"capability=" + value + b"\n" for value in capabilities))


def request(action, data, path=b"Map.uasset", extra=()):
    headers = lines(b"command=" + action + b"\n", b"pathname=" + path + b"\n", *extra)
    return headers + b"".join(packet(data[i:i + 65516]) for i in range(0, len(data), 65516)) + b"0000"


class Reply:
    def __init__(self, data):
        self.data = data
        self.at = 0

    def packet(self):
        assert len(self.data) - self.at >= 4, "truncated response"
        size = int(self.data[self.at:self.at + 4], 16)
        self.at += 4
        if not size:
            return None
        assert 4 <= size <= 65520
        value = self.data[self.at:self.at + size - 4]
        assert len(value) == size - 4
        self.at += size - 4
        return value

    def lines(self):
        values = []
        while (value := self.packet()) is not None:
            values.append(value.removesuffix(b"\n"))
        return values

    def handshake(self, expected=(b"capability=clean", b"capability=smudge")):
        assert self.lines() == [b"git-filter-server", b"version=2"]
        assert self.lines() == list(expected)

    def response(self):
        initial = self.lines()
        if initial == [b"status=error"]:
            return "error", b""
        assert initial == [b"status=success"], initial
        chunks = []
        while (value := self.packet()) is not None:
            chunks.append(value)
        final = self.lines()
        assert final in ([], [b"status=error"]), final
        return ("error" if final else "success"), b"".join(chunks)

    def done(self):
        assert self.at == len(self.data), "unexpected trailing protocol output"


class GitProcess(unittest.TestCase):
    setUp = base.GitCatalog.setUp
    initialize = base.GitCatalog.initialize
    git = base.GitCatalog.git
    cli = base.GitCatalog.cli

    def exchange(self, data, expected=0, *options):
        return self.cli("git", "filter-process", "--checkout-policy", *options,
                        data=data, expected=expected)

    def test_multiple_clean_smudge_empty_and_binary_requests_share_one_session(self):
        content = bytes(range(256)) * 16384
        pointer = self.cli("--store", self.store, "git", "clean", data=content).stdout
        plain = bytes(range(256)) * 4096
        conversation = handshake() + request(b"clean", content) + request(b"smudge", pointer)
        conversation += request(b"smudge", plain, path=b"folder/quoted = name\n.uasset")
        conversation += request(b"clean", b"") + request(b"smudge", b"")
        result = self.exchange(conversation)
        replies = Reply(result.stdout)
        replies.handshake()
        self.assertEqual(replies.response(), ("success", pointer))
        self.assertEqual(replies.response(), ("success", content))
        self.assertEqual(replies.response(), ("success", plain))
        state, empty_pointer = replies.response()
        self.assertEqual(state, "success")
        self.assertTrue(empty_pointer.endswith(b"size 0\n"))
        self.assertEqual(replies.response(), ("success", b""))
        replies.done()
        self.assertEqual(result.stderr, b"")
        progress = self.exchange(handshake() + request(b"clean", content), 0,
                                 "--lang", "fr", "--progress-json")
        replies = Reply(progress.stdout)
        replies.handshake()
        self.assertEqual(replies.response(), ("success", pointer))
        replies.done()
        events = [json.loads(line) for line in progress.stderr.splitlines()]
        self.assertTrue(any(e.get("phase") == "ingesting" for e in events))

    def test_file_error_drains_body_and_keeps_following_requests_usable(self):
        invalid = b"version syrnck-pointer-v1\n" + b"x" * (2 * 1024 * 1024)
        missing = b"version syrnck-pointer-v1\nmanifest blake3:" + b"a" * 64 + b"\nsize 1\n"
        conversation = handshake() + request(b"smudge", invalid) + request(b"smudge", missing)
        conversation += request(b"unknown", b"discarded" * 32768)
        conversation += request(b"smudge", b"discard", extra=(b"command=clean",))
        conversation += request(b"smudge", b"ordinary content")
        result = self.exchange(conversation)
        replies = Reply(result.stdout)
        replies.handshake()
        for _ in range(4):
            self.assertEqual(replies.response(), ("error", b""))
        self.assertEqual(replies.response(), ("success", b"ordinary content"))
        replies.done()
        self.assertIn(b"oversized Syrnck pointer", result.stderr)
        self.assertIn(b"object not found", result.stderr)
        self.assertEqual(json.loads(self.cli("--store", self.store, "refs").stdout), {})

    def test_packet_boundaries_empty_data_and_missing_text_newlines(self):
        conversation = lines(b"git-filter-client", b"version=7", b"version=2")
        conversation += lines(b"capability=smudge", b"capability=future")
        conversation += lines(b"command=smudge", b"pathname=a=b", b"future-key=x=y")
        conversation += b"0004" + packet(b"\0\n0000") + b"0004" + packet(b"binary\xff") + b"0000"
        result = self.exchange(conversation)
        replies = Reply(result.stdout)
        replies.handshake((b"capability=smudge",))
        self.assertEqual(replies.response(), ("success", b"\0\n0000binary\xff"))
        replies.done()

    def test_bad_framing_and_metadata_limits_terminate_without_publication(self):
        valid = handshake()
        headers = lines(b"command=clean", b"pathname=asset")
        cases = [b"", b"000", b"zzzz", b"0001", b"fff1",
                 lines(b"git-filter-client", b"version=1"),
                 lines(b"git-filter-client", b"version=2") + lines(b"capability=delay"),
                 valid + b"000", valid + headers + b"0009abc",
                 valid + headers + packet(b"truncated body without flush"),
                 valid + b"".join(packet(b"x=y") for _ in range(257)) + b"0000",
                 valid + packet(b"a=" + b"x" * 65510) + packet(b"b=" + b"y" * 32) + b"0000"]
        for data in cases:
            with self.subTest(prefix=data[:40], size=len(data)):
                failed = self.exchange(data, 1)
                self.assertNotIn(packet(b"status=success\n"), failed.stdout)
        self.assertEqual(json.loads(self.cli("--store", self.store, "refs").stdout), {})

    def test_skip_process_does_not_open_store_or_decode_key(self):
        self.env.update(SYRNCK_SKIP_SMUDGE="1", SYRNCK_KEY="invalid key")
        missing = self.root / "absent"
        pointer = b"version syrnck-pointer-v1\nmanifest blake3:" + b"b" * 64 + b"\nsize 3\n"
        plain = bytes(range(256)) * 4096
        data = handshake() + request(b"smudge", pointer) + request(b"smudge", plain)
        # Clean still requires a Store; its failure must not break skip-smudge.
        data += request(b"clean", b"data") + request(b"smudge", pointer)
        result = self.cli("--store", missing, "git", "filter-process", "--checkout-policy", data=data,
                          directory=self.root)
        replies = Reply(result.stdout)
        replies.handshake()
        for body in (pointer, plain):
            self.assertEqual(replies.response(), ("success", body))
        self.assertEqual(replies.response(), ("error", b""))
        self.assertEqual(replies.response(), ("success", pointer))
        replies.done()
        self.assertFalse(missing.exists())

    def test_real_git_reuses_one_filter_and_preserves_custom_process_configuration(self):
        (self.repo / ".gitattributes").write_text("*.uasset filter=syrnck -text\n", encoding="utf-8")
        data = bytes(range(256)) * 4096
        for index in range(24):
            (self.repo / f"asset-{index:02}.uasset").write_bytes(data)
        trace = self.root / "trace.json"
        self.env["GIT_TRACE2_EVENT"] = str(trace)
        self.git("add", ".")
        self.env.pop("GIT_TRACE2_EVENT")
        events = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
        starts = [e for e in events if e.get("event") == "child_start" and
                  e.get("child_class") == "subprocess" and
                  any("git filter-process --checkout-policy" in s for s in e.get("argv", []))]
        self.assertEqual(len(starts), 1, starts)
        pointer = self.git("show", ":asset-00.uasset")
        self.assertTrue(pointer.startswith(b"version syrnck-pointer-v1\n"))
        for index in range(24):
            (self.repo / f"asset-{index:02}.uasset").unlink()
        self.git("checkout", "--", ".")
        self.assertTrue(all((self.repo / f"asset-{i:02}.uasset").read_bytes() == data for i in range(24)))
        # Removing the owned resident command preserves the original filters.
        self.git("config", "--local", "--unset", "filter.syrnck.process")
        (self.repo / "fallback.uasset").write_bytes(b"single-file fallback")
        self.git("add", "fallback.uasset")
        self.assertTrue(self.git("show", ":fallback.uasset").startswith(b"version syrnck-pointer-v1\n"))
        self.cli("git", "install")
        self.git("config", "filter.syrnck.process", "custom filter process")
        self.cli("git", "install", expected=1)
        self.assertEqual(self.git("config", "filter.syrnck.process"), b"custom filter process\n")


if __name__ == "__main__":
    unittest.main()
