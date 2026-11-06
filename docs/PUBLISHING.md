# Publishing Syrnck to GitHub

Target: [MrMybal/Syrnck](https://github.com/MrMybal/Syrnck). Preserve the repository's configured visibility unless the owner explicitly requests a change. Publishing this C++ source repository uses ordinary Git; it does not require Syrnck pointers or Git LFS for the current source tree.

## Repository description

Use the text in [GITHUB_DESCRIPTION.txt](GITHUB_DESCRIPTION.txt) for GitHub's **About → Description** field:

> C++20 revision and block storage engine for large assets, with content-defined chunking, deduplication, Git integration, standalone history, and differential file sync.

The primary README is English, with a link to the French README. The description intentionally describes the engine without claiming a finished Git replacement, an Unreal plugin, or implemented Syncthing interoperability.

## License

The original Syrnck code uses the [MIT License](../LICENSE). The root license
is published with the source and installed alongside [third-party notices](../THIRD_PARTY.md).
Upstream components keep their own licenses, including the optional Syncthing
protocol dependency. A normal Git push does not change repository visibility.

## Before the first upload

1. Build and run the tests with `python scripts/build.py build` and `python scripts/build.py test`.
2. Run a publishable-working-tree scan with Gitleaks 8.30.1 or newer:

   ```text
   python scripts/scan_secrets.py --gitleaks PATH_TO_GITLEAKS --report .scratch/gitleaks-working-tree.json
   ```

   The snapshot includes tracked files and non-ignored untracked files, including vendored dependencies. It excludes local tools, caches, stores, generated builds, and reports according to `.gitignore`. Reports use full secret redaction and stay under the ignored `.scratch` directory. The summary records file count, size, and a SHA-256 digest of the scanned names and contents. Git-history scanning is a separate operation.

3. Review the file list with `git status --short` and `git add --dry-run .`. Check that no credentials, local stores, toolchains, or build products would be staged. The scanner rejects files reaching GitHub's 100 MiB Git object limit. `.gitattributes` preserves binary compatibility fixtures and normalizes source text.
4. Confirm access without an upload:

   ```text
   git ls-remote --heads --tags origin
   ```

   If branches already exist, fetch and inspect them before importing. Do not force-push or overwrite an existing history. An empty output with exit code zero means that no branch/tag refs were returned.

## Local setup and initial commit

The local repository has been initialized on `main`, with:

```text
git remote add origin https://github.com/MrMybal/Syrnck.git
```

Do not repeat `git init` or `remote add` if they are already configured; inspect `git remote -v` instead. Once the files have been reviewed:

```text
git add .
python scripts/scan_secrets.py --gitleaks PATH_TO_GITLEAKS --mode staged --report .scratch/gitleaks-staged.json
git diff --cached --stat
git commit -m "Import C++20 engine with English and French interfaces"
python scripts/scan_secrets.py --gitleaks PATH_TO_GITLEAKS --mode history --report .scratch/gitleaks-history.json
```

Only after the scans and tests pass, upload the commit:

```text
git push --dry-run origin main
git push -u origin main
```

These commands do not change repository visibility. A non-fast-forward rejection requires inspection and reconciliation, rather than a forced update. The GitHub Actions workflows run the build/test matrix and secret scan after the upload; confirm their actual results before describing a platform as validated.

## Authentication

Use the existing Git Credential Manager HTTPS login, GitHub CLI login, or an SSH key authorized for this repository. Do not put a token in a remote URL, command argument, committed configuration, or chat message. GitHub CLI is optional; Git alone can perform this import. A connected GitHub app may lack access to a private repository even when local Git credentials have access.

## Reviewed scanner exception

The default Gitleaks generic API-key rule reports the `key_check` verifier in `tests/fixtures/v1/encrypted/config.json`. This is a deterministic hash from the published-format compatibility fixture, not a production encryption key. `.gitleaks.toml` extends all default rules and allows only that exact verifier value at that exact fixture path. Other values in that file and the same value elsewhere remain scanned. Test data and dependencies are not excluded wholesale.

Gitleaks detects patterns; a clean scan is not proof that arbitrary sensitive information is absent. Its role here is a reproducible check of the exact source upload candidate and subsequent Git history.

Reference: [GitHub's documented local-code import procedure](https://docs.github.com/en/migrations/importing-source-code/using-the-command-line-to-import-source-code/adding-locally-hosted-code-to-github?platform=windows), and [Gitleaks configuration](https://github.com/gitleaks/gitleaks#configuration).
