# Workflows

`build.yml` runs on every push and pull request:

- **host** — builds for the build machine, runs the test suite, and renders
  every screen with the simulator, uploading the PNGs as an artifact so a
  reviewer can see what changed visually.
- **device** — lints the installer shell scripts, cross-compiles the static
  ARM binary, runs it under `qemu-user-static` on the target architecture,
  builds the installable packages, and checks their shape.

Pushing a tag starting with `v` publishes the packages as a GitHub release.
