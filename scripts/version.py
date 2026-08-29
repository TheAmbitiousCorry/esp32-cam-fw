"""Injects a version string from git so a build can be identified after it ships.

Falls back to "unknown" outside a repository rather than failing the build.
"""
import subprocess

Import("env")

try:
    version = subprocess.check_output(
        ["git", "describe", "--tags", "--always", "--dirty"],
        stderr=subprocess.DEVNULL, text=True).strip()
except Exception:
    version = "unknown"

print("firmware version: %s" % version)
env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(version))])
