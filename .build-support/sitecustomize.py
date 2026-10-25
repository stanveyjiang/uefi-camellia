"""Compatibility shim for Ubuntu's python3-ntp metadata layout.

The Debian package exposes metadata named ``ntpsec`` from a directory named
``ntp-1.2.3.egg-info``. edk2-pytool-extensions enumerates the metadata name
and then looks it up again by name, which fails on current Python releases.
Returning ``unknown`` only for that diagnostic lookup does not affect builds.
"""

import importlib.metadata


_original_version = importlib.metadata.version


def _version_with_missing_metadata_fallback(name):
    try:
        return _original_version(name)
    except importlib.metadata.PackageNotFoundError:
        return "unknown"


importlib.metadata.version = _version_with_missing_metadata_fallback
