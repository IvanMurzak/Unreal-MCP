"""Print the CI smoke's host .uproject after syncing tests/smoke-host into its persistent folder.

The folder lives beside extension_suite.py's build caches (default_output(): a short C:/wt path on
Windows, .artifacts/tests elsewhere), so the host's Binaries / Intermediate / DerivedDataCache stay
warm across jobs on one runner slot. Python stdlib only.
"""
from extension_suite import smoke_host

if __name__ == "__main__":
    print(smoke_host())
