# hello-ppu-gcm-label-reuse-heavy

This is the heavy form of [hello-ppu-gcm-label-reuse](../hello-ppu-gcm-label-reuse/README.md). It uses the same
source, built with `LABEL_REUSE_HEAVY`:

- a 1024x1024 texture in an 8 MiB mapped arena;
- 256 draws over a 512x512 target;
- a fragment program that makes eight spread texture fetches per pixel, combined with `max()`.

The goal is to keep the RSX reading the arena long after its front end has passed a command label.

**Why it exists.** On the console, the light sample's command-label control was clean, so that workload could not
tell a command label from a back-end label.

**Verdict line.** Besides the light sample's lines, the sample prints `GCM_LABEL_REUSE_A2 conclusive` when:

- the command control is caught, so the window is exposed;
- the backend and texture modes are both clean.

**Waits and crumbs.**

- Every wait is bounded and reported as a failure.
- Each line is also appended to `/dev_hdd0/tmp/gcmlabel-crumb.txt` and fsync'd.

**RPCS3** cannot show the command-label hazard; see [docs/gcm-labels.md](../../../docs/gcm-labels.md#3-what-a-completion-label-guarantees-a2).
