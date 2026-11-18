#!/usr/bin/env python3
"""The stack of the own code in the tasks rs485_bus and sparkplug (issue #227).

The own components (gw_* and main) compile with -fstack-usage and -fcallgraph-info=su
(cmake/gw_warnings.cmake): each object file has a .su file (the frame of each function) and a
.ci file (the call graph of the file, with the frames). This script joins the .ci files of the
build into one call graph and starts at the entry function of each task. A function belongs to a
task when the call graph reaches it from the entry of the task, so a function of gw_core that two
tasks call is in the list of both tasks.

For each task the report gives:
  - the functions of the task with the largest frames;
  - the deepest call path through the own code (the sum of the frames), a lower bound of the
    stack that the task needs;
  - the calls out of the own code (ESP-IDF, newlib, nanopb) with the most own stack above them,
    and the indirect calls (function pointers): their stack is not in the sum.
Then it lists the largest frames of all own functions (the .su files), for -Wstack-usage.

    python3 tools/stack_report.py build > stack.txt
    python3 tools/stack_report.py build --task name=file.c:function   # another task
"""
import argparse
import os
import re
import sys

# name of the task -> (source file, entry function); xTaskCreate*() in gw_bus.c and gw_edge.c
TASKS = {
    "rs485_bus": ("gw_bus.c", "bus_task"),
    "sparkplug": ("gw_edge.c", "edge_task"),
}
INDIRECT = "__indirect_call"
OWN_DIRS = re.compile(r"[/\\]esp-idf[/\\](gw_[^/\\]+|main)[/\\]")

NODE_RE = re.compile(r'^node: \{ title: "([^"]*)" label: "([^"]*)"(.*)\}\s*$')
EDGE_RE = re.compile(r'^edge: \{ sourcename: "([^"]*)" targetname: "([^"]*)"(?: label: "([^"]*)")?')
BYTES_RE = re.compile(r"\\n(\d+) bytes \(([a-z, ]+)\)")
SU_RE = re.compile(r"^(.*):(\d+):(\d+):(\S+)\t(\d+)\t(\S+)\s*$")


def own_files(build, suffix):
    """The files with this suffix of the own components in the build directory."""
    found = []
    for root, _dirs, files in os.walk(os.path.join(build, "esp-idf")):
        for f in files:
            if f.endswith(suffix) and OWN_DIRS.search(os.path.join(root, f)):
                found.append(os.path.join(root, f))
    return sorted(found)


class Graph:
    def __init__(self):
        self.frame = {}   # title -> bytes (functions of the own code only)
        self.qual = {}    # title -> "static", "dynamic", "dynamic,bounded"
        self.where = {}   # title -> "file:line"
        self.name = {}    # title -> function name
        self.calls = {}   # title -> [(target title, call site)]

    def load(self, path):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = NODE_RE.match(line)
                if m:
                    title, label = m.group(1), m.group(2)
                    parts = label.split("\\n")
                    self.name.setdefault(title, parts[0])
                    b = BYTES_RE.search(label)
                    if b:   # a definition; a declaration has no frame
                        self.frame[title] = int(b.group(1))
                        self.qual[title] = b.group(2).replace(" ", "")
                        self.where[title] = parts[1] if len(parts) > 1 else "?"
                    continue
                m = EDGE_RE.match(line)
                if m:
                    self.calls.setdefault(m.group(1), []).append((m.group(2), m.group(3) or "?"))

    def find(self, src, func):
        """The title of a function: "<path>/src:func" if static, else "func"."""
        for t in self.frame:
            if self.name[t] == func and os.path.basename(self.where[t].split(":")[0]) == src:
                return t
        return None


def analyse(g, entry):
    """Depth-first from the entry: the reachable own functions, the back edges (recursion), and
    the longest own path to each function (dist, with the frame of the function itself)."""
    order, seen, onstack, back = [], set(), set(), []
    stack = [(entry, iter(g.calls.get(entry, [])))]
    seen.add(entry)
    onstack.add(entry)
    while stack:
        node, it = stack[-1]
        for tgt, _site in it:
            if tgt not in g.frame:
                continue   # outside the own code, or an indirect call
            if tgt in onstack:
                back.append((node, tgt))
            elif tgt not in seen:
                seen.add(tgt)
                onstack.add(tgt)
                stack.append((tgt, iter(g.calls.get(tgt, []))))
                break
        else:
            stack.pop()
            onstack.discard(node)
            order.append(node)
    order.reverse()   # a topological order of the graph without the back edges
    backset = set(back)
    dist = {entry: g.frame[entry]}
    prev = {entry: None}
    for node in order:
        if node not in dist:
            continue
        for tgt, _site in g.calls.get(node, []):
            if tgt in g.frame and (node, tgt) not in backset:
                d = dist[node] + g.frame[tgt]
                if d > dist.get(tgt, -1):
                    dist[tgt] = d
                    prev[tgt] = node
    return seen, back, dist, prev


def short(path):
    """file.c:line of a "path:line:col" location."""
    m = re.match(r"^(.*?):(\d+)(?::\d+)?$", path)
    if not m:
        return path
    return "%s:%s" % (os.path.basename(m.group(1)), m.group(2))


def report_task(g, out, task, src, func, top):
    entry = g.find(src, func)
    out.append("## Task %s (entry %s in %s)" % (task, func, src))
    out.append("")
    if entry is None:
        out.append("ERROR: no stack data of %s() in %s" % (func, src))
        out.append("")
        return False
    seen, back, dist, prev = analyse(g, entry)
    out.append("%d own functions reach from the entry." % len(seen))
    out.append("")
    out.append("### The largest frames in the task")
    out.append("")
    out.append("| bytes | function | location |")
    out.append("|------:|----------|----------|")
    for t in sorted(seen, key=lambda t: (-g.frame[t], g.name[t]))[:top]:
        q = "" if g.qual[t] == "static" else " (%s)" % g.qual[t]
        out.append("| %d%s | %s | %s |" % (g.frame[t], q, g.name[t], short(g.where[t])))
    out.append("")
    deepest = max(dist, key=lambda t: (dist[t], g.name[t]))
    path = []
    t = deepest
    while t is not None:
        path.append(t)
        t = prev[t]
    path.reverse()
    out.append("### The deepest own call path: %d bytes" % dist[deepest])
    out.append("")
    out.append("The sum of the frames below. The stack in ESP-IDF, newlib and nanopb, and in the "
               "indirect calls, is not in it.")
    out.append("")
    out.append("| bytes | sum | function | location |")
    out.append("|------:|----:|----------|----------|")
    for t in path:
        out.append("| %d | %d | %s | %s |" % (g.frame[t], dist[t], g.name[t], short(g.where[t])))
    out.append("")
    ext = {}
    for t in seen:
        for tgt, site in g.calls.get(t, []):
            if tgt in g.frame:
                continue
            key = "(indirect call)" if tgt == INDIRECT else g.name.get(tgt, tgt)
            if key.startswith("__"):
                continue   # the helpers of the compiler (libgcc, __builtin_*): small leaves
            if dist.get(t, 0) > ext.get(key, (-1,))[0]:
                ext[key] = (dist.get(t, 0), g.name[t], short(site))
    out.append("### The calls out of the own code with the most own stack above them")
    out.append("")
    out.append("| own stack | callee | caller | call site |")
    out.append("|----------:|--------|--------|-----------|")
    for key, (d, caller, site) in sorted(ext.items(), key=lambda kv: (-kv[1][0], kv[0]))[:top]:
        out.append("| %d | %s | %s | %s |" % (d, key, caller, site))
    out.append("")
    if back:
        out.append("Recursion (calls back to a function on the path, counted one time):")
        for a, b in sorted(set(back)):
            out.append("- %s -> %s" % (g.name[a], g.name[b]))
        out.append("")
    return True


def report_all(build, out, top):
    rows = []
    for path in own_files(build, ".su"):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = SU_RE.match(line)
                if m:
                    rows.append((int(m.group(5)), m.group(6), m.group(4),
                                 "%s:%s" % (os.path.basename(m.group(1)), m.group(2))))
    out.append("## The largest frames of all own functions (.su files)")
    out.append("")
    if not rows:
        out.append("ERROR: no .su file in %s" % build)
        out.append("")
        return False
    rows.sort(key=lambda r: (-r[0], r[2]))
    dyn = [r for r in rows if r[1] != "static"]
    out.append("%d functions, %d with a frame that is not static. Functions with a frame of more "
               "than 256 bytes: %d, 512 bytes: %d, 1024 bytes: %d." %
               (len(rows), len(dyn), sum(r[0] > 256 for r in rows),
                sum(r[0] > 512 for r in rows), sum(r[0] > 1024 for r in rows)))
    out.append("")
    out.append("| bytes | function | location |")
    out.append("|------:|----------|----------|")
    for b, q, name, where in rows[:top]:
        out.append("| %d%s | %s | %s |" % (b, "" if q == "static" else " (%s)" % q, name, where))
    out.append("")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("build", help="the build directory (idf.py -B)")
    ap.add_argument("--task", action="append", default=[], metavar="NAME=FILE.c:FUNCTION",
                    help="a task to examine in place of rs485_bus and sparkplug")
    ap.add_argument("--top", type=int, default=15, help="rows in each table (default 15)")
    args = ap.parse_args()
    tasks = dict(TASKS)
    if args.task:
        tasks = {}
        for spec in args.task:
            m = re.match(r"^([^=]+)=([^:]+):(\w+)$", spec)
            if not m:
                ap.error("--task %s: write NAME=FILE.c:FUNCTION" % spec)
            tasks[m.group(1)] = (m.group(2), m.group(3))
    cis = own_files(args.build, ".ci")
    if not cis:
        sys.exit("no .ci file of the own components in %s: build with gw_warnings.cmake "
                 "(-fcallgraph-info=su)" % args.build)
    g = Graph()
    for path in cis:
        g.load(path)
    out = ["# Stack of the own code (issue #227)", "",
           "Frames in bytes, from the -fstack-usage and -fcallgraph-info=su data of %d object "
           "files. Inline functions are in the frame of their caller." % len(cis), ""]
    ok = True
    for task, (src, func) in tasks.items():
        ok = report_task(g, out, task, src, func, args.top) and ok
    ok = report_all(args.build, out, args.top) and ok
    print("\n".join(out))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
