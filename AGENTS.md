# AGENTS.md — deltafs

This file supplements the repo-root `AGENTS.md`. Both apply when working in
`fs/deltafs`.

deltafs is based on overlayfs from Linux 6.8.0 and adds support for hot-swapping
the layer stack to provide filesystem version control.

## Repository layout — scope all searches

This tree is a full Linux 6.8 checkout (~83k files). deltafs work lives in
only three directories:

- `fs/overlayfs/` — deltafs kernel module source (fork of overlayfs)
- `tools/deltafs/` — userspace tools and tests (p1 … p7, acceptance)
- `docs/` — design docs and test/reproduction guides

Everything else is stock upstream kernel, reference only. Blind whole-tree
searches waste context on upstream noise, so:

- Never run rg/grep/find without a path argument scoping it to the
  directories above, e.g. `rg pattern fs/overlayfs/`.
- For upstream context (VFS helpers, struct definitions), read the specific
  known file (e.g. `include/linux/fs.h`) or search one specific directory,
  e.g. `rg pattern include/linux/`.
- Matches outside the directories above are upstream kernel code — never
  treat or cite them as deltafs code.

## Testing

- After development, perform **static testing only** in this environment:
  build the kernel module / userspace tools, run static analyzers, and check
  that code compiles and links cleanly. Do not attempt to run the deltafs
  kernel module here — this environment cannot boot the custom kernel.
- **Functional testing is done by the user in a QEMU virtual machine.** The
  agent does not have a VM and must not assume any in-kernel behavior has been
  verified by running it.
- Therefore, every deliverable must include a **complete test handoff** the
  user can follow inside QEMU:
  - how to build the kernel and the userspace test tools (`tools/deltafs/`);
  - how to boot the QEMU VM with the built kernel and disk image;
  - how to mount deltafs and run each test (`p1` … `p7`, acceptance, etc.),
    with exact commands and expected output;
  - how to collect logs / dmesg / failure diagnostics.
  Keep this in sync under `docs/` (e.g. the reproduction / test guide) whenever
  tests or interfaces change.
