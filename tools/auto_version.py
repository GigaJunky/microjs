# PlatformIO build hook (see [env] in platformio.ini). PlatformIO's hooks have to be Python, so this
# only runs the real script, tools/auto-version.js, with Node. Without Node the build carries on and
# the version simply isn't bumped.
import os, shutil, subprocess

Import("env")  # noqa: F821 - provided by PlatformIO
root = env["PROJECT_DIR"]  # noqa: F821
node = shutil.which("node")
if node:
    subprocess.run([node, os.path.join(root, "tools", "auto-version.js"), root], check=False)
else:
    print("auto-version: Node.js isn't installed, so the version wasn't checked (https://nodejs.org)")
