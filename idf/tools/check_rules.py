#!/usr/bin/env python3
"""Checks of three rules of CLAUDE.md on the code of the project (issue #228).

    noalloc  REL-2: no allocation in the polling, encoding or publish path.
    jsonlock #106: each use of cJSON is under gw_port_json_lock().
    rs485    Only the rs485_bus task touches the RS485 UART and the Modbus master.

The host tests (idf/test/host/CMakeLists.txt) run each check on the host build of gw_core and
nanopb, and again on a small bad example (the self-test), which the check must refuse. The
objects compile with -fcallgraph-info=su: next to each object <src>.c.o, GCC writes the call graph
of the file, <src>.c.ci (the same format that tools/stack_report.py reads). The checks join the
graphs of all the objects into one call graph. A static function has the title "<path>:<name>",
a global function the title "<name>".

The call graph has the direct calls that are in the generated code. Its limits:
  - A call through a function pointer is "__indirect_call": the checks do not follow it.
  - A function that is only an argument (an address, not a call) is not an edge. The noalloc
    check also examines the undefined symbols of each listed object with nm, for this case.
  - The graph is of the host build. The firmware port (gw_port_esp.c) and the IDF components are
    not in it. The source checks of jsonlock and rs485 examine the IDF components as text.
  - The calls into the C library are not followed (for example, a printf that allocates).

    python3 check_rules.py noalloc  --objects "<obj;obj>" --nm nm --list noalloc_path.txt
    python3 check_rules.py jsonlock --objects "<obj;obj>" --root <idf dir> --core gw_core
    python3 check_rules.py rs485    --objects "<obj;obj>" --root <idf dir> --core gw_core
                                    --owners gw_bus,gw_rs485 --exceptions rs485_exceptions.txt

Each check prints one line for each violation and exits with 1, or prints a summary and exits
with 0.
"""
import argparse
import os
import re
import subprocess
import sys

NODE_RE = re.compile(r'^node: \{ title: "([^"]*)" label: "([^"]*)"(.*)\}\s*$')
EDGE_RE = re.compile(r'^edge: \{ sourcename: "([^"]*)" targetname: "([^"]*)"(?: label: "([^"]*)")?')

# The functions of these files are the platform boundary: the host port is not the firmware.
PORT_FILES = re.compile(r"^gw_port_\w+\.c$")


class Graph:
    """The call graph of a set of objects (their .ci files)."""

    def __init__(self, objects):
        self.src = {}     # title -> source file of the definition
        self.calls = {}   # title -> [(callee title, call site)]
        self.callers = {}  # title -> set of caller titles (defined functions only)
        for obj in objects:
            ci = re.sub(r"\.o(bj)?$", ".ci", obj)
            if not os.path.isfile(ci):
                sys.exit(f"{ci}: no call graph; compile {obj} with -fcallgraph-info=su")
            self._load(ci)
        if not self.src:
            sys.exit("the call graph has no function")
        for t, cs in self.calls.items():
            for c, _site in cs:
                if c in self.src:
                    self.callers.setdefault(c, set()).add(t)

    def _load(self, path):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = NODE_RE.match(line)
                if m:
                    if "shape" not in m.group(3):   # a definition (a declaration is an ellipse)
                        parts = m.group(2).split("\\n")
                        where = parts[1] if len(parts) > 1 else "?"
                        self.src[m.group(1)] = where.rsplit(":", 2)[0]
                        self.calls.setdefault(m.group(1), [])
                    continue
                m = EDGE_RE.match(line)
                if m:
                    self.calls.setdefault(m.group(1), []).append((m.group(2), m.group(3) or "?"))

    def file(self, t):
        return os.path.basename(self.src[t])

    def name(self, t):
        return t.rsplit(":", 1)[-1]

    def show(self, t):
        return f"{self.file(t)}:{self.name(t)}" if t in self.src else t

    def is_global(self, t):
        return ":" not in t

    def callees(self, t):
        return self.calls.get(t, [])


def site(s):
    """A call site "path/file.c:line:col" as "file.c:line"."""
    m = re.match(r"(.*):(\d+):\d+$", s)
    return f"{os.path.basename(m.group(1))}:{m.group(2)}" if m else s


def chain(prev, t, g):
    out = []
    while t is not None:
        out.append(g.show(t))
        t = prev.get(t)
    return " -> ".join(reversed(out))


# ---------- source text: comments and literals removed ----------

TOKEN_RE = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def code_of(path):
    """The text of a C file without comments, string literals and character literals. The line
    numbers stay the same."""
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()

    def blank(m):
        s = m.group(0)
        if s.startswith('"'):
            return '""'
        if s.startswith("'"):
            return "' '"
        return " " + "\n" * s.count("\n")
    return text, TOKEN_RE.sub(blank, text)


def sources(root, skip):
    """The C files of the components (except the names in skip) and of main."""
    found = []
    comp = os.path.join(root, "components")
    for c in sorted(os.listdir(comp)) if os.path.isdir(comp) else []:
        if c not in skip and os.path.isdir(os.path.join(comp, c)):
            found += walk_c(os.path.join(comp, c))
    if os.path.isdir(os.path.join(root, "main")):
        found += walk_c(os.path.join(root, "main"))
    return found


def walk_c(d):
    out = []
    for r, _dirs, files in os.walk(d):
        out += [os.path.join(r, f) for f in sorted(files) if f.endswith((".c", ".h"))]
    return sorted(out)


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def rel(path, root):
    return os.path.relpath(path, root)


# ---------- noalloc ----------

# The allocators, cJSON (it allocates through its hooks) and the JSON lock (gw_port.h: never in
# the polling or publish path). gw_config_alloc and gw_config_release call the allocator of
# gw_config through a function pointer, so the call graph does not show malloc after them;
# gw_config_json_hooks installs that allocator into cJSON.
ALLOC_RE = re.compile(r"^(malloc|calloc|realloc|reallocarray|free|strdup|strndup|aligned_alloc|"
                      r"posix_memalign|memalign|valloc|asprintf|vasprintf|cJSON_\w+|"
                      r"gw_port_json_lock|gw_config_alloc|gw_config_release|"
                      r"gw_config_json_hooks)$")


def read_list(path):
    """Lines "<file.c> <reason>"; a line that starts with "#" is a comment. A file without a
    reason is an error."""
    entries = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 1)
            if len(parts) < 2:
                sys.exit(f"{path}:{n}: '{parts[0]}' has no reason")
            entries.append((parts[0], parts[1]))
    return entries


def check_noalloc(args):
    objects = [o for o in args.objects.split(";") if o]
    g = Graph(objects)
    listed = read_list(args.list)
    bad = []
    by_name = {}
    for o in objects:
        by_name.setdefault(re.sub(r"\.o(bj)?$", "", os.path.basename(o)), []).append(o)
    for src, _reason in listed:
        objs = by_name.get(src, [])
        if len(objs) != 1:
            bad.append(f"{args.list}: {src} is {len(objs)} objects of the build, not 1")
            continue
        # nm -u: also an allocator that is only an argument or a value (no call)
        out = subprocess.run([args.nm, "-u", objs[0]], capture_output=True, text=True, check=True)
        for line in out.stdout.splitlines():
            sym = line.split()[-1] if line.split() else ""
            if ALLOC_RE.match(sym):
                bad.append(f"nm: {src} refers to {sym}")
    # the call graph: each function of the listed files, and each own function that it calls
    files = {s for s, _r in listed}
    roots = [t for t in g.src if g.file(t) in files]
    prev = {t: None for t in roots}
    todo = list(roots)
    while todo:
        t = todo.pop()
        for c, s in g.callees(t):
            if ALLOC_RE.match(c):
                bad.append(f"call graph: {chain(prev, t, g)} -> {c} ({site(s)})")
            elif c in g.src and c not in prev and not PORT_FILES.match(g.file(c)):
                prev[c] = t
                todo.append(c)
    if bad:
        print("REL-2: allocation in the polling, encoding or publish path (issue #228):")
        print("\n".join("  " + b for b in bad))
        return 1
    outside = sorted({g.file(t) for t in prev if g.file(t) not in files})
    print(f"REL-2: {len(listed)} files, {len(prev)} functions reached, no allocation "
          f"(also reached in: {', '.join(outside) or '-'})")
    return 0


# ---------- jsonlock ----------

LOCK, UNLOCK = "gw_port_json_lock", "gw_port_json_unlock"
CJSON_RE = re.compile(r"^cJSON_\w+$")


def public_names(root, core):
    """The words in the public headers of the core component (its API for the other ones)."""
    names = set()
    for h in walk_c(os.path.join(root, "components", core, "include")):
        names |= set(re.findall(r"\b[A-Za-z_]\w*\b", code_of(h)[1]))
    return names


def check_jsonlock(args):
    objects = [o for o in args.objects.split(";") if o]
    g = Graph(objects)
    bad = []
    # 1. outside the core component: no cJSON at all (the source text)
    for f in sources(args.root, {args.core}):
        text, code = code_of(f)
        for m in re.finditer(r'#\s*include\s*[<"]cJSON\.h[>"]', text):
            bad.append(f"{rel(f, args.root)}:{line_of(text, m.start())}: includes cJSON.h")
        for m in re.finditer(r"\bcJSON\w*", code):
            bad.append(f"{rel(f, args.root)}:{line_of(code, m.start())}: {m.group(0)} outside "
                       f"{args.core}")
    # 2. in the core component: each function that calls cJSON holds the lock, or each path
    # to it from an entry passes a function that takes the lock
    takers = {t for t in g.src if any(c == LOCK for c, _s in g.callees(t))}
    for t in sorted(takers):
        if not any(c == UNLOCK for c, _s in g.callees(t)):
            bad.append(f"{g.show(t)} calls {LOCK} and never {UNLOCK}")
    api = public_names(args.root, args.core)
    # entries: no caller in the graph (also a function called only through a pointer), or a
    # global function of the public headers (the other components can call it)
    entries = [t for t in g.src
               if not g.callers.get(t) or (g.is_global(t) and g.name(t) in api)]
    prev = {}
    todo = []
    for t in entries:
        if t not in takers:
            prev[t] = None
            todo.append(t)
    while todo:   # the functions that a path without the lock reaches
        t = todo.pop()
        for c, _s in g.callees(t):
            if c in g.src and c not in prev and c not in takers:
                prev[c] = t
                todo.append(c)
    reached = set(entries)   # the functions that any path from an entry reaches
    todo = list(entries)
    while todo:
        t = todo.pop()
        for c, _s in g.callees(t):
            if c in g.src and c not in reached:
                reached.add(c)
                todo.append(c)
    users = 0
    for t in sorted(g.src):
        uses = [(c, s) for c, s in g.callees(t) if CJSON_RE.match(c)]
        users += bool(uses)
        if not uses or t in takers:
            continue
        c, s = uses[0]
        if t in prev:
            bad.append(f"{chain(prev, t, g)} calls {c} ({site(s)}) without {LOCK}")
        elif t not in reached:   # only a cycle without an entry calls it (a function pointer)
            bad.append(f"{g.show(t)} calls {c} ({site(s)}), and no entry reaches it")
    if bad:
        print("#106: cJSON without the JSON lock (issue #228):")
        print("\n".join("  " + b for b in bad))
        return 1
    print(f"#106: {users} functions call cJSON, {len(takers)} take the lock; no cJSON outside "
          f"{args.core}")
    return 0


# ---------- rs485 ----------

BUS_CALL_RE = re.compile(r"\b(uart_\w+|mb_master_\w+)\s*\(")


def read_exceptions(path):
    """Lines "<file relative to the idf dir> <function> <reason>"."""
    out = {}
    if not path:
        return out
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 2)
            if len(parts) < 3:
                sys.exit(f"{path}:{n}: give the file, the function and the reason")
            out[(parts[0], parts[1])] = parts[2]
    return out


def check_rs485(args):
    objects = [o for o in args.objects.split(";") if o]
    g = Graph(objects)
    owners = set(args.owners.split(","))
    exceptions = read_exceptions(args.exceptions)
    used = set()
    bad = []
    # the functions of the core component that reach the Modbus master: only the bus task may
    # call them. The master itself (mb_master_*) is in the set by its name.
    master = {t for t in g.src if g.name(t).startswith("mb_master_")}
    bus = set(master)
    changed = True
    while changed:
        changed = False
        for t in g.src:
            if t not in bus and any(c in bus or c.startswith("mb_master_")
                                    for c, _s in g.callees(t)):
                bus.add(t)
                changed = True
    for t in sorted(g.src):
        for c, s in g.callees(t):
            if c.startswith("uart_"):
                bad.append(f"{g.show(t)} calls {c} ({site(s)}): the core component has no UART")
    bus_api = sorted(g.name(t) for t in bus - master if g.is_global(t))
    names = re.compile(r"\b(" + "|".join(map(re.escape, bus_api)) + r")\b") if bus_api else None
    # the API of the owners (gw_rs485_*, not the queue API gw_bus_*): only the owners call it
    owner_api = set()
    for o in owners - {"gw_bus"}:
        for h in walk_c(os.path.join(args.root, "components", o, "include")):
            owner_api |= set(re.findall(r"\b(" + re.escape(o) + r"_\w+)\s*\(", code_of(h)[1]))
    api_re = (re.compile(r"\b(" + "|".join(map(re.escape, sorted(owner_api))) + r")\s*\(")
              if owner_api else None)
    for f in sources(args.root, owners | {args.core}):
        _text, code = code_of(f)
        r = rel(f, args.root).replace(os.sep, "/")
        hits = [(m.start(), m.group(1)) for m in BUS_CALL_RE.finditer(code)]
        for rx in (names, api_re):
            if rx:
                hits += [(m.start(), m.group(1)) for m in rx.finditer(code)]
        for pos, name in sorted(hits):
            if (r, name) in exceptions:
                used.add((r, name))
                continue
            bad.append(f"{r}:{line_of(code, pos)}: {name} outside {', '.join(sorted(owners))}")
    for e in sorted(set(exceptions) - used):
        bad.append(f"{args.exceptions}: the exception {e[0]} {e[1]} is not used; remove it")
    if bad:
        print("Only the rs485_bus task touches the RS485 UART and the Modbus master (issue #228):")
        print("\n".join("  " + b for b in bad))
        return 1
    print(f"RS485: uart_* and mb_master_* only in {', '.join(sorted(owners))}; "
          f"{len(bus_api)} functions of {args.core} reach the master "
          f"({', '.join(bus_api)}), and only the owners call them")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("check", choices=["noalloc", "jsonlock", "rs485"])
    ap.add_argument("--objects", required=True, help="the objects, separated by ';'")
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--list", help="noalloc: the files of the path, with the reasons")
    ap.add_argument("--root", help="jsonlock, rs485: the idf directory")
    ap.add_argument("--core", default="gw_core", help="the component of the call graph")
    ap.add_argument("--owners", default="gw_bus,gw_rs485")
    ap.add_argument("--exceptions", help="rs485: the accepted calls, with the reasons")
    args = ap.parse_args()
    if args.check == "noalloc":
        return check_noalloc(args)
    if args.check == "jsonlock":
        return check_jsonlock(args)
    return check_rs485(args)


if __name__ == "__main__":
    sys.exit(main())
