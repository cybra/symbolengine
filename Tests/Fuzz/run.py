#!/usr/bin/env python3
"""run.py <build folder> <seconds> [target ...]: runs each libFuzzer target for that long, from what Seeds keeps and what
earlier runs in the same work folder found, stopping at the first failure, with its input kept to be added to Seeds.

The memory a run may take is held down, as an input that makes a target take more is itself a finding. Every target
starts from at least one seed and has to run some inputs, so a run can't pass without having looked at anything."""
import base64, os, re, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.abspath(os.path.join(here, "..", ".."))
extra_seeds = {"audio_files": [os.path.join(repo, "Modules", "files", "Tests", "Data")]}


def targets_in(build):
    found = []
    for folder, _, files in os.walk(os.path.join(build, "Tests", "Fuzz")):
        for name in files:
            path = os.path.join(folder, name)
            if name.startswith("symbolengine_fuzz_") and os.access(path, os.X_OK) and "." not in name:
                found.append(path)
    return sorted(found)


def main():
    build, seconds = sys.argv[1], int(sys.argv[2])
    wanted = sys.argv[3:]
    work = os.path.join(build, "fuzz-work")
    failures = 0
    programs = [p for p in targets_in(build) if not wanted or os.path.basename(p).split("symbolengine_fuzz_")[1] in wanted]
    if not programs:
        print("No fuzz targets in", build)
        return 1

    for program in programs:
        name = os.path.basename(program).split("symbolengine_fuzz_")[1]
        corpus = os.path.join(work, name)
        artifacts = os.path.join(work, "found", name) + os.sep
        os.makedirs(corpus, exist_ok=True)
        os.makedirs(artifacts, exist_ok=True)
        seeds = [os.path.join(here, "Seeds", name)] + extra_seeds.get(name, [])
        command = [program, f"-max_total_time={seconds}", "-rss_limit_mb=2048", "-malloc_limit_mb=1024", "-timeout=30",
                   "-print_final_stats=1", f"-artifact_prefix={artifacts}", corpus, *seeds]
        print("===", name, flush=True)
        run = subprocess.run(command, capture_output=True, text=True, errors="replace")
        output = run.stderr + run.stdout
        executed = re.search(r"stat::number_of_executed_units:\s+(\d+)", output)
        count = int(executed.group(1)) if executed else 0
        print(f"exit {run.returncode}, {count} inputs run")
        kept = sorted(os.listdir(artifacts))
        if run.returncode != 0 or count == 0:
            failures += 1
            print("\n".join(output.splitlines()[-40:]))
            for k in kept:
                bytes_kept = open(os.path.join(artifacts, k), "rb").read()
                print(f"Kept {os.path.join(artifacts, k)}, {len(bytes_kept)} bytes:")
                if len(bytes_kept) <= 65536:
                    print(base64.b64encode(bytes_kept).decode())
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
