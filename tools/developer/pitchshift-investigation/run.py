#!/usr/bin/env python3
"""Stage and, only on explicit request, run the archived DSP probes."""

import argparse
import ast
import os
from pathlib import Path
import subprocess
import sys

SOURCES = (
    "probe.cpp",
    "timing.cpp",
    "render.cpp",
    "generate.py",
    "analyze.py",
    "transients.py",
    "transition.cpp",
)
OLD_TMP = "/tmp/mixxx-pitchshift-investigation"


def inside(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root", type=Path, default=Path(__file__).resolve().parents[3]
    )
    parser.add_argument(
        "--output", type=Path, default=Path("/tmp/mixxx-pitchshift-archive")
    )
    parser.add_argument(
        "--run",
        action="store_true",
        help="compile and run all historical probes",
    )
    args = parser.parse_args()

    repo = args.repo_root.resolve(strict=True)
    here = Path(__file__).resolve().parent
    output = args.output.resolve()
    if inside(output, repo):
        parser.error("output directory must be outside the repository")
    if output.exists():
        parser.error(
            f"refusing to overwrite existing output directory: {output}"
        )

    output.mkdir(parents=True)
    staged = output / "staged"
    staged.mkdir()
    root_literal = str(repo)
    for name in SOURCES:
        original = here / name
        if not original.is_file():
            parser.error(f"missing archived source: {original}")
        text = original.read_text(encoding="utf-8")
        text = text.replace(OLD_TMP, str(output))
        old_repo = (
            "/var/home/kyvernitria/Applications/"
            "mixxx-signalsmith-memory-cues"
        )
        text = text.replace(old_repo, root_literal)
        if name.endswith(".py"):
            # Python joins adjacent literals before evaluating paths; handle
            # the line wrapping applied by the repository formatter too.
            class RelocateStrings(ast.NodeTransformer):
                def visit_Constant(self, node):
                    if isinstance(node.value, str):
                        node.value = node.value.replace(old_repo, root_literal)
                        node.value = node.value.replace(OLD_TMP, str(output))
                    return node

            text = ast.unparse(RelocateStrings().visit(ast.parse(text))) + "\n"
        # transition.cpp includes timing.cpp by its former absolute /tmp path.
        text = text.replace(
            f'#include "{output}/timing.cpp"', '#include "timing.cpp"'
        )
        (staged / name).write_text(text, encoding="utf-8")

    print(f"Staged archived sources in {staged}")
    if not args.run:
        print(
            "Not compiled or executed; pass --run only for an explicit "
            "investigation."
        )
        return 0

    binaries = output / "bin"
    renders = output / "renders"
    binaries.mkdir()
    renders.mkdir()
    cxx = os.environ.get("CXX", "g++")
    common = [
        cxx,
        "-std=c++17",
        "-O3",
        "-DNDEBUG",
        f"-I{repo / 'lib/signalsmith-stretch'}",
        f"-I{repo / 'lib'}",
    ]

    def compile_source(
        source: str, binary: str, libraries: tuple[str, ...] = ()
    ) -> Path:
        target = binaries / binary
        subprocess.run(
            [*common, str(staged / source), *libraries, "-o", str(target)],
            check=True,
        )
        return target

    timing = compile_source("timing.cpp", "timing", ("-lrubberband",))
    probe = compile_source("probe.cpp", "probe")
    render = compile_source(
        "render.cpp", "render", ("-lrubberband", "-lsndfile")
    )
    transition = compile_source(
        "transition.cpp", "transition", ("-lrubberband",)
    )

    subprocess.run([str(timing)], check=True)
    subprocess.run([str(probe)], check=True)
    subprocess.run([sys.executable, str(staged / "generate.py")], check=True)
    fixture_dir = repo / "src/test/stems/stem02"
    inputs = {
        name: output / f"{name}.wav"
        for name in ("tones", "vowel", "transients")
    }
    inputs.update(
        {
            name: fixture_dir / f"{name}.wav"
            for name in ("trance_mainmix", "04-vocal", "01-drum")
        }
    )
    for fixture, source in inputs.items():
        render_dir = renders / fixture
        render_dir.mkdir()
        subprocess.run([str(render), str(source), str(render_dir)], check=True)
    subprocess.run([sys.executable, str(staged / "analyze.py")], check=True)
    subprocess.run([sys.executable, str(staged / "transients.py")], check=True)
    subprocess.run([str(transition)], check=True)
    print(f"Historical probe outputs are isolated in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
