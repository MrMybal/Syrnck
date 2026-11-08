# Reference daemon build fixture

This module builds the upstream daemon for live BEP interoperability tests. Its
Syncthing version must match `bindings/syncthing/go.mod`. The checksums include
the pinned upstream module's complete cross-platform checksum set and its own
source checksum. Only packages required by the selected host build are fetched.

Use `python scripts/build_syncthing_reference.py --download`. The binary uses
the upstream `noassets` tag and is a test dependency; it is not installed with
the application.

See the [English](../../Documentation/Documentation.html#/en/syncthing/reference-daemon)
or [French](../../Documentation/Documentation.html#/fr/syncthing/reference-daemon)
local manual for the actual daemon test and its limits.
