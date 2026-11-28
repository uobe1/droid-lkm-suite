#!/usr/bin/env python3
"""Check that every input a workflow references actually exists and is buildable.

Written after failures where a workflow referenced something the repository
did not provide: a probe compile line added to only one of the two jobs that
build probe binaries, and a `make ... modules` target the referenced Makefile
did not define.  Both were findable before pushing.  This script is the
mechanical check:

  1. every workflow parses as YAML;
  2. every `make -C <dir> ... <target>` has a Makefile in <dir> that defines
     <target> (or delegates through it);
  3. every `cc ... -o <out> <src>` source file exists;
  4. every `cp <src> <dst>` source exists (repo-relative paths only);
  5. every `gh run download -n <name>` artifact name is uploaded by some
     workflow, so a download cannot silently target nothing.

Exit code 1 on any finding.  Pure Python 3, no third-party imports.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORKFLOWS = ROOT / ".github" / "workflows"

problems: list[str] = []


def rel(p: Path) -> str:
    try:
        return str(p.relative_to(ROOT))
    except ValueError:
        return str(p)


def workflow_texts() -> dict[Path, str]:
    texts: dict[Path, str] = {}
    for path in sorted(WORKFLOWS.glob("*.yml")):
        texts[path] = path.read_text(encoding="utf-8")
    if not texts:
        problems.append("no workflows found under .github/workflows")
    return texts


def check_yaml(texts: dict[Path, str]) -> None:
    try:
        import yaml  # noqa: PLC0415
    except ImportError:
        problems.append("PyYAML missing: cannot validate workflow syntax")
        return
    for path, text in texts.items():
        try:
            yaml.safe_load(text)
        except Exception as exc:  # noqa: BLE001
            problems.append(f"{rel(path)}: YAML does not parse: {exc}")


MAKE_RE = re.compile(r"make\s+-C\s+\"?([^\"\s]+)\"?((?:\s+[^\s\\]+)*)")


def check_make_targets(texts: dict[Path, str]) -> None:
    for path, text in texts.items():
        for line in text.splitlines():
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            for match in MAKE_RE.finditer(line):
                directory, tail = match.group(1), match.group(2)
                # Only repo-relative directories can be checked locally; skip
                # shell variables such as "$KDIR" or "$SRC".
                # "$GITHUB_WORKSPACE/tests/x" -> "tests/x"; an unresolvable
                # path such as "$KDIR" is not a repo directory and is skipped.
                stripped_dir = re.sub(r"^\$\{?[A-Za-z_][A-Za-z0-9_]*\}?/", "", directory)
                if "$" in stripped_dir:
                    continue
                directory = stripped_dir
                target_dir = (ROOT / directory).resolve()
                if not target_dir.is_dir():
                    problems.append(
                        f"{rel(path)}: make -C {directory} but that directory is missing"
                    )
                    continue
                makefile = target_dir / "Makefile"
                if not makefile.is_file():
                    problems.append(
                        f"{rel(path)}: make -C {directory} but {rel(makefile)} is missing"
                    )
                    continue
                targets = makefile_targets(makefile)
                wanted = [t for t in tail.split() if t and not t.startswith("-")]
                wanted = [t for t in wanted if "=" not in t]
                if not wanted:
                    continue
                # the last bare token is the target when options precede it
                target = wanted[-1]
                if target not in targets:
                    problems.append(
                        f"{rel(path)}: make -C {directory} {target} but "
                        f"{rel(makefile)} defines only {sorted(targets)}"
                    )


def makefile_targets(makefile: Path) -> set[str]:
    targets: set[str] = set()
    for line in makefile.read_text(encoding="utf-8").splitlines():
        if line.startswith("\t") or not line.strip() or line.lstrip().startswith("#"):
            continue
        head = line.split(":", 1)[0]
        for token in head.split():
            if token and not token.startswith(".") and "=" not in token and "%" not in token:
                targets.add(token)
    return targets


# Only repository inputs are checked.  Anything else (dnsk_core.ko, boot/kernel,
# modules/*.ko, netpkg/., ...) is produced earlier in the same job or by another
# workflow, so its absence from the tree is expected.
SOURCE_ROOTS = ("tests/", "features/", "core/", "scripts/", "profiles/", "ksu/")

COMPILER_RE = re.compile(r"\b(?:[a-z0-9_-]*-)?(?:gcc|clang|cc)\b[^;]*?\s-o\s")
CP_RE = re.compile(r"\bcp\s+(?:-[a-zA-Z]+\s+)*([^\s;&|]+)\s+")


def logical_lines(text: str):
    """Join shell continuations so a wrapped command is inspected as one line."""
    buffer = ""
    for raw in text.splitlines():
        line = raw.rstrip()
        if line.endswith("\\"):
            buffer += line[:-1] + " "
            continue
        buffer += line
        yield buffer
        buffer = ""
    if buffer:
        yield buffer


def repo_input(token: str) -> str | None:
    """Return the repo-relative path if this token names a repository input."""
    if not token or token.startswith("-") or "$" in token or token.startswith("/"):
        return None
    if "*" in token or token.startswith("\\"):
        return None
    if token.endswith((".ko", ".o", ".so")):
        return None  # build output, produced in the same step
    if not token.startswith(SOURCE_ROOTS):
        return None
    return token


def check_compile_and_copy(texts: dict[Path, str]) -> None:
    for path, text in texts.items():
        for line in logical_lines(text):
            if line.strip().startswith("#"):
                continue
            if COMPILER_RE.search(line):
                tokens = line.split()
                skip_next = False
                for token in tokens:
                    if skip_next:
                        skip_next = False          # the -o output name
                        continue
                    if token in ("-o",):
                        skip_next = True
                        continue
                    source = repo_input(token)
                    if source and not (ROOT / source).is_file():
                        problems.append(
                            f"{rel(path)}: compiles {source} but that source file is missing"
                        )
            for match in CP_RE.finditer(line):
                source = repo_input(match.group(1).rstrip("/"))
                if source and not (ROOT / source).exists():
                    problems.append(
                        f"{rel(path)}: copies {source} but that path is missing"
                    )


DOWNLOAD_RE = re.compile(r"gh\s+run\s+download\s+(?:\S+\s+)?-n\s+(\S+)")
UPLOAD_RE = re.compile(r"name:\s*(\S+)\s*$")


def check_artifacts(texts: dict[Path, str]) -> None:
    uploaded: set[str] = set()
    for text in texts.values():
        if "upload-artifact" not in text:
            continue
        # artifact names appear in the block following an upload-artifact step
        for block in text.split("upload-artifact")[1:]:
            for name in UPLOAD_RE.findall(block[:600]):
                uploaded.add(name)
    for path, text in texts.items():
        for name in DOWNLOAD_RE.findall(text):
            if name.startswith("$"):
                continue
            if uploaded and name not in uploaded:
                problems.append(
                    f"{rel(path)}: downloads artifact {name!r} but no workflow uploads it"
                )



JOB_RE = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$")


def workflow_jobs(text: str) -> dict[str, str]:
    """Split a workflow into job blocks keyed by job id (2-space indented keys)."""
    jobs: dict[str, list[str]] = {}
    current: str | None = None
    for line in text.splitlines():
        match = JOB_RE.match(line)
        if match:
            current = match.group(1)
            jobs[current] = []
            continue
        if current is not None:
            jobs[current].append(line)
    return {name: "\n".join(body) for name, body in jobs.items()}


def check_copied_outputs_exist(texts: dict[Path, str]) -> None:
    """A bare `cp <name>... <dest>` source must be produced in the same job.

    This is the listxattr_probe failure: its compile line was added to only one
    of the two jobs that copy the binary, so the other job died with
    `cp: cannot stat 'listxattr_probe'`.  A cp may name several sources.
    """
    skip_suffixes = (".ko", ".o", ".so", ".tar.gz", ".tgz", ".zip", ".gz")
    for path, text in texts.items():
        for job, body in workflow_jobs(text).items():
            built: set[str] = set()
            lines = list(logical_lines(body))
            for line in lines:
                if not COMPILER_RE.search(line):
                    continue
                tokens = line.split()
                for index, token in enumerate(tokens):
                    if token == "-o" and index + 1 < len(tokens):
                        built.add(tokens[index + 1])
            for line in lines:
                stripped = line.strip()
                # drop shell guards so "cp a b || true" and
                # "if [ ... ]; then cp a b" reduce to the cp itself
                if "${{" in stripped or "$(" in stripped:
                    continue  # workflow expression or command substitution
                for guard in ("||", "&&"):
                    stripped = stripped.split(guard)[0].strip()
                stripped = stripped.split(";")[-1].strip()
                if not (stripped == "cp" or stripped.startswith("cp ")):
                    continue
                tokens = [t for t in stripped.split()[1:] if not t.startswith("-")]
                if len(tokens) < 2:
                    continue
                sources = tokens[:-1]
                for source in sources:
                    if "/" in source or "*" in source or source.endswith(skip_suffixes):
                        continue
                    if "$" in source or source.startswith(("\"", "'")):
                        continue  # shell variable or quoted expansion
                    if source not in built and not (ROOT / source).exists():
                        problems.append(
                            f"{rel(path)}[{job}]: copies {source!r} but this job never "
                            f"builds it (compiled here: {sorted(built) or 'none'})"
                        )



SCRIPT_CALL_RE = re.compile(r"\b(?:sh|bash|source)\s+(?:\.\s+)?([^\s;&|]+)")
INPUT_USE_RE = re.compile(r"(?<![-\w.])inputs\.([A-Za-z_][A-Za-z0-9_-]*)")
NEEDS_RE = re.compile(r"^\s+needs:\s*(.+)$")


def dispatch_inputs(text: str) -> set[str]:
    """Names declared under on.workflow_dispatch.inputs."""
    declared: set[str] = set()
    inside_dispatch = False
    inside_inputs = False
    base_indent = 0
    for raw in text.splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip())
        stripped = raw.strip()
        if stripped.startswith("workflow_dispatch:"):
            inside_dispatch = True
            base_indent = indent
            continue
        if inside_dispatch and indent <= base_indent and not stripped.startswith("inputs:"):
            inside_dispatch = False
            inside_inputs = False
        if inside_dispatch and stripped.startswith("inputs:"):
            inside_inputs = True
            inputs_indent = indent
            continue
        if inside_inputs and indent == inputs_indent + 2 and stripped.endswith(":"):
            declared.add(stripped[:-1].strip())
        elif inside_inputs and indent <= inputs_indent:
            inside_inputs = False
    return declared


def check_dispatch_inputs(texts: dict[Path, str]) -> None:
    """Every inputs.X used must be declared, so a typo cannot silently no-op."""
    for path, text in texts.items():
        declared = dispatch_inputs(text)
        used = set(INPUT_USE_RE.findall(text))
        if not used and not declared:
            continue
        for name in sorted(used - declared):
            problems.append(
                f"{rel(path)}: references inputs.{name} but workflow_dispatch "
                f"declares only {sorted(declared) or 'nothing'}"
            )


def check_script_references(texts: dict[Path, str]) -> None:
    """sh/bash/source of a repository script must resolve."""
    for path, text in texts.items():
        for line in logical_lines(text):
            if line.strip().startswith("#"):
                continue
            for match in SCRIPT_CALL_RE.finditer(line):
                token = match.group(1)
                if "$" in token or token in ("-c", "-e", "-x") or token.startswith("-"):
                    continue
                if not token.startswith(SOURCE_ROOTS):
                    continue
                if not (ROOT / token).is_file():
                    problems.append(
                        f"{rel(path)}: runs {token} but that script is missing"
                    )


def check_needs(texts: dict[Path, str]) -> None:
    """A needs: entry must name a job that exists in the same workflow."""
    for path, text in texts.items():
        jobs = set(workflow_jobs(text))
        in_jobs = False
        for raw in text.splitlines():
            if raw.strip() == "jobs:":
                in_jobs = True
                continue
            if not in_jobs:
                continue
            match = NEEDS_RE.match(raw)
            if not match:
                continue
            value = match.group(1).strip().strip("[]")
            for name in [v.strip().strip("'\"") for v in value.split(",") if v.strip()]:
                if name and name not in jobs:
                    problems.append(
                        f"{rel(path)}: needs {name!r} but jobs are {sorted(jobs)}"
                    )


def main() -> int:
    texts = workflow_texts()
    check_yaml(texts)
    check_make_targets(texts)
    check_compile_and_copy(texts)
    check_artifacts(texts)
    check_copied_outputs_exist(texts)
    check_dispatch_inputs(texts)
    check_script_references(texts)
    check_needs(texts)
    if problems:
        print(f"check-ci-inputs: {len(problems)} problem(s)")
        for item in problems:
            print(f"  FAIL {item}")
        return 1
    print(f"check-ci-inputs: clean ({len(texts)} workflow(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
