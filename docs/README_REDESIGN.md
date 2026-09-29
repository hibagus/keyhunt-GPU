# C03: README and usage documentation

The README follows the requested
[Best-README-Template](https://github.com/othneildrew/Best-README-Template)
structure: identity/status, contents, project description, build prerequisites,
usage, roadmap, contributing, licensing, contact and acknowledgments. The template
was consulted on 2026-09-29; its project-specific examples and license were not
copied as keyhunt facts.

The front page now distinguishes the tested CPU implementation from future
HIP/CUDA, checkpoints and coordination. Its links identify this repository
(`hibagus/keyhunt-GPU`) and credit upstream keyhunt. No release badges, GPU speed
numbers, invented maintainer contacts or screenshots are used.

Current commands are in [USAGE.md](USAGE.md), formats and limitations in
[MODES.md](MODES.md), and build options remain in [BUILD.md](BUILD.md). The
[historical README](HISTORICAL_README.md) preserves the prior long explanations,
examples and attribution, with a warning that they are historical. Root README
anchors for the old major sections remain as a navigation entry to that archive.
`BSGSD.md` remains at its established path, with a current-status note.

Validation executes the finite address quickstart against `tests/1to32.txt` and
the synthetic BSGS command against scalar `0x100001`, in temporary working
directories. Expected scalar-1 and BSGS results, exit codes and result files are
checked. The release build and 38-case suite were validated in C02; C03 changes
documentation and its validation automation, not production search code. Local links, heading anchors and code fences are checked.
Raw example results are saved in [C03_EXAMPLES.json](baselines/C03_EXAMPLES.json).

`tools/check_docs.py` checks Markdown links/anchors/fences without a network
request. `tests/integration/readme_examples.py` runs both finite examples. The
new `.github/workflows/cpu.yml` runs these checks plus the CPU release build and
baseline on Ubuntu 22.04, without GPU SDKs. Its checkout action is pinned to
[the verified v4.2.2 commit](https://github.com/actions/checkout/commit/11bd71901bbe5b1630ceea73d27597364c9af683).
The commands were exercised locally; the hosted workflow has not run because no
commit was pushed. The historical documents' source text and attribution remain,
with whitespace normalized for Markdown review.
