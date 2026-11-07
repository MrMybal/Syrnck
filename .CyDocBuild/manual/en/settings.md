---
id: settings
title: Repository settings
description: Repository settings guide.
section: Storage
order: 540
parent: storage
revision: 1
---

# Repository settings {#repository-settings}

Each store can persist local ingestion and packing preferences in `settings.json`.
This optional versioned file is separate from the physical `config.json`, keys,
references and logical objects. Old stores without it keep their previous
defaults. Merely opening or inspecting such a store does not create the file.

```text
syrnck config show
syrnck config set --workers 4 --buffer-mib 32 --pack-max-mib 512 --pack-max-objects 65536
syrnck config reset
```

Updates validate the complete proposed configuration and atomically replace the
file under the store's exclusive lock. Unspecified fields retain their values.
Unknown fields, unsupported versions, invalid types and out-of-range values are
rejected. Settings contain no credentials, encryption keys or persisted permission
to compact/delete data. Symlinks and non-regular settings files are refused.
`config reset` can replace a malformed regular settings file with defaults.

Defaults are one ingest worker, a 32 MiB buffer budget, 512 MiB per new pack and
65,536 objects per pack. Workers range from 1 to 64, buffers from 1 to 1,024 MiB,
and the existing pack limits still apply. The ingest budget must also fit the
selected maximum chunk size: default CDC needs at least 4 MiB. The budget covers
queued raw block bytes, rather than total process memory.

`put`, `native commit` and `git clean` read these preferences, then apply explicit
`--workers` and `--buffer-mib` overrides. `pack` applies `--max-mib` and
`--max-objects` similarly.
Overrides do not rewrite settings. Compaction and deletion remain explicit
actions. Cache budgets still use their existing per-process flags.

In C++, use `Store::settings()` and `Store::set_settings()`. The two-argument
`ingest(input, chunking)` and zero-argument `pack()` use repository defaults.
Passing `IngestOptions` or `PackOptions` explicitly supplies the entire operation's
options. The default ingestion path is also used by native commits, Git clean
filters and C/Go file ingestion. Library names, JSON fields, IDs and pointers
remain language-independent; the CLI provides English and French diagnostics.

Settings do not change existing data or object formats and are not retention
roots. Integrity auditing and explicit reset remain available when preferences
are malformed. Different stores, including Git's common store and a standalone
`.syrnck`, have independent preferences.

For Git filters, use `--store <common-store>` with the path printed by `git install`.
An ordinary standalone `config` command otherwise addresses `.syrnck` by default.

See [progress and cancellation](doc:operations) for CLI output modes, Ctrl+C and
the C++ callback interface.
