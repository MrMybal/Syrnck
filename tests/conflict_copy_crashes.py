"""Terminate conflict-copy merges and verify keyless recovery of both versions."""
import unittest
import owned_checkout_crashes as owned


class ConflictCopyCrashes(owned.OwnedCheckoutCrashes):
    def scenarios(self):
        return [(encrypted, scope, "conflict-merge", False)
                for encrypted in (False, True)
                for scope in ("primary", "linked", "partial", "detached")]


if __name__ == "__main__":
    unittest.main()
