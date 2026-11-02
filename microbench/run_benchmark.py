#!/usr/bin/env python3
"""
Re-run the atomsnap benchmarks described in README.md, summarize the
results, and optionally rewrite the numbers in README.md.

Usage (from anywhere):
    python3 microbench/rerun_benchmarks.py                  # run + report
    python3 microbench/rerun_benchmarks.py --update-readme  # + rewrite README

What it does:
  1. Rebuilds libatomsnap and every benchmark binary (make -B).
  2. Runs every configuration listed in README "Performance Comparison".
     By default only the atomsnap side is re-run, because the baselines
     (shared_ptr, shared_mutex, spinlock, urcu) do not call atomsnap and
     are not affected by changes in atomsnap.c. Use --all-backends to
     re-run the baselines too.
  3. Reads the previous numbers from the current README tables, merges
     the new measurements, and prints a report (previous vs new).
  4. With --update-readme, regenerates every table and every number-bearing
     sentence in the Performance Comparison section. Before writing, it
     checks that regenerating from the *previous* numbers reproduces the
     current README exactly; if not (the README was edited by hand), it
     does not touch README.md.
  5. Sentences that make a qualitative claim (e.g. "No starvation",
     "remains constant") are never rewritten; they are checked against the
     data and listed in the report as OK or NEEDS REVIEW.

Results are cached in microbench/rerun_results.json, so an interrupted run
can be resumed by running the same command again (use --fresh to ignore it).
Only the Python standard library is used.
"""

import argparse
import copy
import csv
import difflib
import io
import json
import os
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# Experiment definitions (must match README "Performance Comparison")
# ---------------------------------------------------------------------------

B1_DURATION = 100          # README: "Duration: 100 seconds per test"
B2_DURATION = 15           # README: "duration=15s"
B1_CFGS = ["1/1", "2/2", "4/4", "8/8"]
B1_A_BACKENDS = ["shared_ptr", "atomsnap"]
B1_BC_BACKENDS = ["shared_mutex", "shared_ptr", "spinlock", "atomsnap"]
B1_C_CFGS = {"1R/16W": (16, 1), "16R/1W": (1, 16)}   # (writers, readers)
B1_PROGRAMS = {
	"shared_ptr": "shared_ptr_example",
	"atomsnap": "atomsnap_example",
	"shared_mutex": "mutex_example",
	"spinlock": "spinlock_example",
}

B2_BACKENDS = ["urcu", "atomsnap"]
B2_SHARDS = [1, 8]
B2_A_CS_US = ["0", "10", "100"]
B2_B_RATES = {"unlimited": 0, "100k": 100000, "500k": 500000,
	"1M": 1000000, "2M": 2000000}
B2_B_CS_NS = 200
B2_READERS = 16
B2_WRITERS = 1
B2_PAYLOAD = 64

SECTION_START = "# Performance Comparison"
SECTION_END = "# Critical Usage Rules"

# ---------------------------------------------------------------------------
# Formatting helpers
# ---------------------------------------------------------------------------

def rd(x, nd):
	"""Round the way the table shows it (string formatting), back to float."""
	return float(f"{x:.{nd}f}")


def commas(x):
	return f"{int(round(x)):,}"


def m1(x):
	return f"{x / 1e6:.1f}M"


def m_rank(x):
	v = x / 1e6
	return f"{v:.1f}M" if v >= 0.1 else f"{v:.3f}M"


def m_auto(x):
	if x >= 1e6:
		return f"{x / 1e6:.1f}M"
	return f"{round(x / 1e3)}K"


def ratio(a, b):
	return f"{a / b:.1f}x"


def ratio_short(a, b):
	s = f"{a / b:.1f}"
	return (s[:-2] if s.endswith(".0") else s) + "x"


def rng(values, nd):
	"""'lo–hi' using the displayed precision, or a single value if equal."""
	lo = f"{min(values):.{nd}f}"
	hi = f"{max(values):.{nd}f}"
	return lo if lo == hi else f"{lo}–{hi}"


def trend(first, last):
	return "increases" if last > first else "decreases"


def same(a, b):
	"""True when the ratio would print as 1.0x."""
	return f"{max(a, b) / min(a, b):.1f}" == "1.0"


def is_constant(values, tol=0.05, abs_tol=0.5):
	"""Spread within 5% of the minimum, or within 0.5 (MB) in absolute terms."""
	spread = max(values) - min(values)
	return spread <= max(tol * min(values), abs_tol)

# ---------------------------------------------------------------------------
# Markdown table helpers
# ---------------------------------------------------------------------------

def split_row(line):
	cells = line.strip().strip("|").split("|")
	return [c.strip() for c in cells]


def is_sep_row(cells):
	return all(re.fullmatch(r":?-+:?", c) for c in cells if c != "")


def sep_kind(cell):
	if cell.startswith(":") and cell.endswith(":"):
		return "c"
	if cell.startswith(":"):
		return "l"
	if cell.endswith(":"):
		return "r"
	return "-"


def normalize_table(lines):
	out = []
	for line in lines:
		cells = split_row(line)
		if is_sep_row(cells):
			out.append("|" + "|".join(sep_kind(c) for c in cells) + "|")
		else:
			out.append("|" + "|".join(cells) + "|")
	return out


def render_table(headers, rows, aligns, style):
	"""style: 'pad' (aligned columns), 'plain' (|---|), 'hdr' (dashes = header)."""
	cols = len(headers)
	if style == "pad":
		widths = [max(len(headers[i]), *(len(r[i]) for r in rows))
			for i in range(cols)]

		def fmt(cells):
			return "| " + " | ".join(cells[i].ljust(widths[i])
				for i in range(cols)) + " |"

		def sep(i):
			w = widths[i]
			if aligns[i] == "c":
				return ":" + "-" * w + ":"
			if aligns[i] == "l":
				return ":" + "-" * (w + 1)
			return "-" * (w + 2)

		lines = [fmt(headers), "|" + "|".join(sep(i) for i in range(cols)) + "|"]
		lines += [fmt(r) for r in rows]
		return lines

	def plain(cells):
		return "| " + " | ".join(cells) + " |"

	if style == "plain":
		sep = "|" + "|".join("---" for _ in range(cols)) + "|"
	else:
		sep = "|" + "|".join("-" * (len(h) + 2) for h in headers) + "|"
	return [plain(headers), sep] + [plain(r) for r in rows]

# ---------------------------------------------------------------------------
# README section parsing
# ---------------------------------------------------------------------------

HEADING_GROUP = {
	"### Experiment A: Consistency Only": "b1A",
	"### Experiment B: Consistency + Monotonicity": "b1B",
	"### Experiment C: Unbalanced Reader/Writer Ratio": "b1C",
	"### Benchmark 1 Summary": "b1S",
	"### Experiment A: Reader critical-section delay sweep": "b2A",
	"#### A-1) shards=1": "b2A",
	"#### A-2) shards=8": "b2A",
	"### Experiment B: Writer rate limiting sweep": "b2B",
	"#### B-1) shards=1": "b2B",
	"#### B-2) shards=8": "b2B",
	"### Benchmark 2 Summary": "b2S",
}

# (heading, caption) -> table id
TABLE_IDS = {
	("### Experiment A: Consistency Only", "Reader Throughput (ops/sec)"): "b1A_reader",
	("### Experiment A: Consistency Only", "Writer Throughput (ops/sec)"): "b1A_writer",
	("### Experiment B: Consistency + Monotonicity", "Reader Throughput (ops/sec)"): "b1B_reader",
	("### Experiment B: Consistency + Monotonicity", "Writer Throughput (ops/sec)"): "b1B_writer",
	("### Experiment C: Unbalanced Reader/Writer Ratio", "Configuration: 1 Reader, 16 Writers"): "b1C_1R/16W",
	("### Experiment C: Unbalanced Reader/Writer Ratio", "Configuration: 16 Readers, 1 Writer"): "b1C_16R/1W",
	("### Benchmark 1 Summary", None): "b1S",
	("#### A-1) shards=1", None): "b2A_1",
	("#### A-2) shards=8", None): "b2A_8",
	("#### B-1) shards=1", None): "b2B_1",
	("#### B-2) shards=8", None): "b2B_8",
	("### Benchmark 2 Summary", None): "b2S",
}

B2_COLS = {"r(M/s)": "r", "w(M/s)": "w", "RSS(MB)": "rss", "lat(us)": "lat"}

# Rows of the current README that do not follow from its own tables.
# They are regenerated from the data instead of being treated as hand edits.
#   b2S "RSS at CS=100us": README shows atomsnap 20.6–20.8MB, but the A-1/A-2
#   tables give 20.8MB for both shard settings at CS=100us.
KNOWN_README_ERRORS = {("b2S", "RSS at CS=100us")}


def section_bounds(lines):
	start = end = None
	for i, line in enumerate(lines):
		if line.rstrip() == SECTION_START and start is None:
			start = i
		elif line.rstrip() == SECTION_END and start is not None:
			end = i
			break
	if start is None or end is None:
		raise SystemExit("README: could not find the Performance Comparison section")
	return start, end


def walk_section(lines):
	"""Yield ('table', key, group, [lines]) or ('line', None, group, line)."""
	heading = None
	caption = None
	i = 0
	while i < len(lines):
		line = lines[i]
		if line.startswith("|"):
			j = i
			while j < len(lines) and lines[j].startswith("|"):
				j += 1
			yield ("table", (heading, caption), HEADING_GROUP.get(heading),
				lines[i:j])
			caption = None
			i = j
			continue
		if line.startswith("#"):
			heading = line.rstrip()
			caption = None
		elif line.strip():
			caption = line.strip()
		yield ("line", None, HEADING_GROUP.get(heading), line)
		i += 1


def parse_readme_data(sec_lines):
	data = {"b1": {"A": {"reader": {}, "writer": {}},
			"B": {"reader": {}, "writer": {}},
			"C": {}},
		"b2": {"A": {1: {}, 8: {}}, "B": {1: {}, 8: {}}}}
	found = set()
	for kind, key, _, block in walk_section(sec_lines):
		if kind != "table" or key not in TABLE_IDS:
			continue
		tid = TABLE_IDS[key]
		found.add(tid)
		rows = [split_row(l) for l in block]
		header, body = rows[0], [r for r in rows[2:]]
		if tid.startswith("b1A") or tid.startswith("b1B"):
			exp = tid[2]
			kind_rw = tid.split("_")[1]
			for col, name in enumerate(header[1:], start=1):
				be = name.replace("std::", "")
				data["b1"][exp][kind_rw][be] = {
					r[0]: int(r[col].replace(",", "")) for r in body}
		elif tid.startswith("b1C"):
			cfg = tid.split("_")[1]
			d = {"reader": {}, "writer": {}}
			for col, name in enumerate(header[1:], start=1):
				for r in body:
					rw = "reader" if r[0].startswith("Reader") else "writer"
					d[rw][name] = int(r[col].replace(",", ""))
			data["b1"]["C"][cfg] = d
		elif tid.startswith("b2A") or tid.startswith("b2B"):
			exp = tid[2]
			shards = int(tid.split("_")[1])
			for r in body:
				row = {}
				for col, name in enumerate(header[1:], start=1):
					be, metric = name.split(" ", 1)
					row.setdefault(be, {})[B2_COLS[metric]] = float(r[col])
				data["b2"][exp][shards][r[0]] = row
	missing = set(TABLE_IDS.values()) - found
	if missing:
		raise SystemExit("README: tables not found: " + ", ".join(sorted(missing)))
	return data

# ---------------------------------------------------------------------------
# Rendering: tables
# ---------------------------------------------------------------------------

def render_tables(D):
	b1, b2 = D["b1"], D["b2"]
	t = {}
	for exp, bes in (("A", B1_A_BACKENDS), ("B", B1_BC_BACKENDS)):
		for rw in ("reader", "writer"):
			hdr = ["Readers/Writers"] + [("std::" + b if exp == "A" and b == "shared_ptr" else b) for b in bes]
			rows = [[cfg] + [commas(b1[exp][rw][b][cfg]) for b in bes] for cfg in B1_CFGS]
			t[f"b1{exp}_{rw}"] = render_table(hdr, rows, ["c"] * len(hdr), "pad")
	for cfg in B1_C_CFGS:
		hdr = ["Metric"] + B1_BC_BACKENDS
		rows = [["Reader ops/sec"] + [commas(b1["C"][cfg]["reader"][b]) for b in B1_BC_BACKENDS],
			["Writer ops/sec"] + [commas(b1["C"][cfg]["writer"][b]) for b in B1_BC_BACKENDS]]
		t[f"b1C_{cfg}"] = render_table(hdr, rows, ["l"] + ["c"] * 4, "pad")

	def top(vals):
		return max(vals, key=vals.get)

	def top_all(exp, rw):
		tops = {top({b: b1[exp][rw][b][c] for b in b1[exp][rw]}) for c in B1_CFGS}
		return tops.pop() if len(tops) == 1 else "varies by config"

	rows = [["Consistency Only (all configs)", top_all("A", "reader"), top_all("A", "writer")]]
	for cfg in ("1/1", "8/8"):
		rows.append([f"Consistency + Monotonicity {cfg}",
			top({b: b1["B"]["reader"][b][cfg] for b in B1_BC_BACKENDS}),
			top({b: b1["B"]["writer"][b][cfg] for b in B1_BC_BACKENDS})])
	for cfg in B1_C_CFGS:
		rows.append([cfg, top(b1["C"][cfg]["reader"]), top(b1["C"][cfg]["writer"])])
	t["b1S"] = render_table(["Scenario", "Highest Reader Throughput",
		"Highest Writer Throughput"], rows, ["-"] * 3, "hdr")

	fmt = {"r": 2, "w": 3, "rss": 1, "lat": 3}
	for exp, keys, order, first in (
			("A", B2_A_CS_US, ["r", "w", "rss", "lat"], "CS(us)"),
			("B", list(B2_B_RATES), ["r", "w", "lat", "rss"], "updates/s")):
		inv = {v: k for k, v in B2_COLS.items()}
		hdr = [first] + [f"{be} {inv[m]}" for be in B2_BACKENDS for m in order]
		for sh in B2_SHARDS:
			rows = [[k] + [f"{b2[exp][sh][k][be][m]:.{fmt[m]}f}"
				for be in B2_BACKENDS for m in order] for k in keys]
			t[f"b2{exp}_{sh}"] = render_table(hdr, rows, ["-"] * len(hdr), "plain")

	t["b2S"] = render_table(["Metric", "urcu", "atomsnap"],
		b2_summary_rows(D), ["-"] * 3, "hdr")
	return t


def b2v(D, exp, sh, key, be, m):
	nd = {"r": 2, "w": 3, "rss": 1, "lat": 3}[m]
	return rd(D["b2"][exp][sh][key][be][m], nd)


def b2_summary_rows(D):
	def per(be, f):
		return [f(be, sh) for sh in B2_SHARDS]

	rows = []
	rows.append(["Read-side latency (CS=0us)"] +
		[rng(per(be, lambda b, s: b2v(D, "A", s, "0", b, "lat")), 3) + "us" for be in B2_BACKENDS])
	rows.append(["Reader throughput (CS=0us, shards=8)"] +
		[f"{b2v(D, 'A', 8, '0', be, 'r'):.1f}M/s" for be in B2_BACKENDS])
	rows.append(["Writer throughput (CS=100us)"] +
		[rng(per(be, lambda b, s: b2v(D, "A", s, "100", b, "w")), 1) + "M/s" for be in B2_BACKENDS])
	for cs in ("0", "100"):
		rows.append([f"RSS at CS={cs}us"] +
			[rng(per(be, lambda b, s: b2v(D, "A", s, cs, b, "rss")), 1) + "MB" for be in B2_BACKENDS])
	cells = []
	for be in B2_BACKENDS:
		allv = [b2v(D, "A", s, c, be, "rss") for s in B2_SHARDS for c in B2_A_CS_US]
		if is_constant(allv):
			cells.append("None")
		else:
			ratios = [b2v(D, "A", s, "100", be, "rss") / b2v(D, "A", s, "0", be, "rss")
				for s in B2_SHARDS]
			cells.append(rng(ratios, 1) + "x increase")
	rows.append(["RSS variation with CS delay"] + cells)
	rows.append(["RSS variation with update rate"] +
		[rng([b2v(D, "B", s, k, be, "rss") for s in B2_SHARDS for k in B2_B_RATES], 1) + "MB"
			for be in B2_BACKENDS])
	return rows

# ---------------------------------------------------------------------------
# Rendering: number-bearing sentences
# Each rule: (group, regex matching the current line, render function)
# ---------------------------------------------------------------------------

def b1_line_rules():
	def A(D, rw):
		a = D["b1"]["A"][rw]["atomsnap"]
		s = D["b1"]["A"][rw]["shared_ptr"]
		return (f"- `atomsnap` {rw} throughput {trend(a['1/1'], a['8/8'])} with thread count "
			f"({m1(a['1/1'])} → {m1(a['8/8'])}). `shared_ptr` {rw} throughput "
			f"{trend(s['1/1'], s['8/8'])} ({m1(s['1/1'])} → {m1(s['8/8'])}).")

	def A3(D):
		r, w = D["b1"]["A"]["reader"], D["b1"]["A"]["writer"]
		return (f"- At 8/8 configuration, `atomsnap` achieves "
			f"{ratio(r['atomsnap']['8/8'], r['shared_ptr']['8/8'])} reader throughput and "
			f"{ratio(w['atomsnap']['8/8'], w['shared_ptr']['8/8'])} writer throughput "
			f"compared to `shared_ptr`.")

	def rank(D, rw, cfg):
		v = {b: D["b1"]["B"][rw][b][cfg] for b in B1_BC_BACKENDS}
		order = sorted(v, key=v.get, reverse=True)
		return " > ".join(f"`{b}` ({m_rank(v[b])})" for b in order)

	def B3(D):
		w = D["b1"]["B"]["writer"]
		tops = {c: max(B1_BC_BACKENDS, key=lambda b: w[b][c]) for c in B1_CFGS}
		if len(set(tops.values())) == 1:
			head = f"`{tops['1/1']}` is highest across all configurations."
		else:
			head = "the highest varies by configuration (" + ", ".join(
				f"{c}: `{b}`" for c, b in tops.items()) + ")."
		return f"- Writer throughput: {head} At 8/8: {rank(D, 'writer', '8/8')}."

	def B4(D):
		r = D["b1"]["B"]["reader"]
		sp = r["spinlock"]
		t_a = trend(r["atomsnap"]["1/1"], r["atomsnap"]["8/8"])
		t_m = trend(r["shared_mutex"]["1/1"], r["shared_mutex"]["8/8"])
		if t_a == t_m:
			tail = f"`atomsnap` and `shared_mutex` reader throughput {t_a[:-1]}."
		else:
			tail = (f"`atomsnap` reader throughput {t_a}; "
				f"`shared_mutex` reader throughput {t_m}.")
		return (f"- `spinlock` reader throughput {trend(sp['1/1'], sp['8/8'])} from "
			f"{m1(sp['1/1'])} to {m1(sp['8/8'])} as thread count increases. {tail}")

	def B5(D):
		v = D["b1"]["B"]["writer"]["shared_mutex"]["8/8"]
		return (f"- `shared_mutex` writer throughput drops to near-zero ({commas(v)} ops/sec) "
			f"at 8/8 due to writer starvation under read-heavy contention.")

	def C1(D):
		c = D["b1"]["C"]["1R/16W"]
		rt = max(c["reader"], key=c["reader"].get)
		wt = max(c["writer"], key=c["writer"].get)
		why = " due to reader priority in `rwlock` semantics" if rt == "shared_mutex" else ""
		return (f"- 1R/16W: `{rt}` provides highest reader throughput ({m_auto(c['reader'][rt])})"
			f"{why}. `{wt}` provides highest writer throughput ({m_auto(c['writer'][wt])}).")

	def C2(D):
		c = D["b1"]["C"]["16R/1W"]
		order = sorted(c["reader"], key=c["reader"].get, reverse=True)
		r1, r2 = order[0], order[1]
		wt = max(c["writer"], key=c["writer"].get)
		w_part = (f"`{wt}` also provides highest writer throughput" if wt == r1
			else f"`{wt}` provides highest writer throughput")
		return (f"- 16R/1W: `{r1}` provides highest reader throughput ({m_auto(c['reader'][r1])}), "
			f"{ratio_short(c['reader'][r1], c['reader'][r2])} higher than the next "
			f"(`{r2}` {m_auto(c['reader'][r2])}). {w_part} ({m_auto(c['writer'][wt])}).")

	def C3(D):
		v = D["b1"]["C"]["16R/1W"]["writer"]["shared_mutex"]
		unit = "op/sec" if int(round(v)) == 1 else "ops/sec"
		return (f"- `shared_mutex` writer throughput drops to {commas(v)} {unit} at 16R/1W, "
			f"indicating complete writer starvation.")

	def C4(D):
		c = D["b1"]["C"]["16R/1W"]["reader"]
		avg = (c["spinlock"] + c["shared_ptr"]) / 2
		return (f"- `spinlock` and `shared_ptr` show similar reader throughput "
			f"(~{round(avg / 1e6)}M) at 16R/1W.")

	return [
		("b1A", r"^- `atomsnap` reader throughput ", lambda D: A(D, "reader")),
		("b1A", r"^- `atomsnap` writer throughput ", lambda D: A(D, "writer")),
		("b1A", r"^- At 8/8 configuration,", A3),
		("b1B", r"^- Reader throughput at 1/1:", lambda D: f"- Reader throughput at 1/1: {rank(D, 'reader', '1/1')}."),
		("b1B", r"^- Reader throughput at 8/8:", lambda D: f"- Reader throughput at 8/8: {rank(D, 'reader', '8/8')}."),
		("b1B", r"^- Writer throughput: ", B3),
		("b1B", r"^- `spinlock` reader throughput ", B4),
		("b1B", r"^- `shared_mutex` writer throughput drops to near-zero", B5),
		("b1C", r"^- 1R/16W:", C1),
		("b1C", r"^- 16R/1W:", C2),
		("b1C", r"^- `shared_mutex` writer throughput drops to ", C3),
		("b1C", r"^- `spinlock` and `shared_ptr` show similar", C4),
	]


def b2_line_rules():
	def lat_line(D, be):
		return (f"- `{be}` read latency: {b2v(D, 'A', 1, '0', be, 'lat'):.3f}us (shards=1), "
			f"{b2v(D, 'A', 8, '0', be, 'lat'):.3f}us (shards=8).")

	def overhead(D):
		a = b2v(D, "A", 1, "0", "atomsnap", "lat")
		u = b2v(D, "A", 1, "0", "urcu", "lat")
		if same(a, u):
			return "- `urcu` and `atomsnap` read-side overhead are similar at shards=1."
		if u <= a:
			return f"- `urcu` read-side overhead is {a / u:.1f}x lower than `atomsnap` at shards=1."
		return f"- `atomsnap` read-side overhead is {u / a:.1f}x lower than `urcu` at shards=1."

	def thr_shards(D, be):
		r1 = b2v(D, "A", 1, "0", be, "r")
		r8 = b2v(D, "A", 8, "0", be, "r")
		k = r8 / r1
		chg = f"{k:.1f}x increase" if k >= 1 else f"{1 / k:.1f}x decrease"
		return f"- `{be}` reader throughput: {r1:.1f}M (shards=1) → {r8:.1f}M (shards=8), a {chg}."

	def urcu_rss(D, cs):
		return (f"- `urcu` RSS at CS={cs}us: {b2v(D, 'A', 1, cs, 'urcu', 'rss'):.1f}MB (shards=1), "
			f"{b2v(D, 'A', 8, cs, 'urcu', 'rss'):.1f}MB (shards=8).")

	def atom_rss_A(D):
		v = [b2v(D, "A", s, c, "atomsnap", "rss") for s in B2_SHARDS for c in B2_A_CS_US]
		return f"- `atomsnap` RSS: {rng(v, 1)}MB across all CS values and shard configurations."

	def urcu_rss_growth(D):
		k = [b2v(D, "A", s, "100", "urcu", "rss") / b2v(D, "A", s, "0", "urcu", "rss")
			for s in B2_SHARDS]
		verb = "increases" if all(x >= 1 for x in k) else "changes"
		return (f"- `urcu` RSS {verb} {k[0]:.1f}x (shards=1) and {k[1]:.1f}x (shards=8) "
			f"as CS increases from 0us to 100us.")

	def writer_cs100(D, sh):
		a = b2v(D, "A", sh, "100", "atomsnap", "w")
		u = b2v(D, "A", sh, "100", "urcu", "w")
		if same(a, u):
			win = "`atomsnap` and `urcu` are similar."
		else:
			win = (f"`atomsnap` is {a / u:.1f}x higher." if a >= u
				else f"`urcu` is {u / a:.1f}x higher.")
		return f"- At CS=100us (shards={sh}): `atomsnap` {a:.2f}M/s, `urcu` {u:.2f}M/s. {win}"

	def urcu_rss_rate(D, key, label):
		return (f"- `urcu` RSS at {label}: {b2v(D, 'B', 1, key, 'urcu', 'rss'):.1f}MB (shards=1), "
			f"{b2v(D, 'B', 8, key, 'urcu', 'rss'):.1f}MB (shards=8).")

	def atom_rss_B(D):
		v = [b2v(D, "B", s, k, "atomsnap", "rss") for s in B2_SHARDS for k in B2_B_RATES]
		return f"- `atomsnap` RSS: {rng(v, 1)}MB across all update rates."

	def urcu_varies(D):
		a = b2v(D, "B", 1, "100k", "urcu", "rss")
		b = b2v(D, "B", 1, "unlimited", "urcu", "rss")
		return (f"- `urcu` RSS varies by {max(a, b) / min(a, b):.1f}x between 100k and unlimited "
			f"(shards=1). `atomsnap` RSS is constant.")

	def rate_thr(D, be):
		return (f"- `{be}` at 100k: {b2v(D, 'B', 8, '100k', be, 'r'):.1f}M/s. "
			f"`{be}` at unlimited: {b2v(D, 'B', 8, 'unlimited', be, 'r'):.1f}M/s.")

	def rate_win(D, key, label):
		a = b2v(D, "B", 8, key, "atomsnap", "r")
		u = b2v(D, "B", 8, key, "urcu", "r")
		if same(a, u):
			return f"- `atomsnap` and `urcu` reader throughput are similar at {label}."
		w, l, hi, lo = ("atomsnap", "urcu", a, u) if a >= u else ("urcu", "atomsnap", u, a)
		return f"- `{w}` reader throughput is {hi / lo:.1f}x higher than `{l}` at {label}."

	def char_urcu(D):
		a = b2v(D, "A", 1, "0", "atomsnap", "lat")
		u = b2v(D, "A", 1, "0", "urcu", "lat")
		if same(a, u):
			lat = "Read-side latency similar to `atomsnap` at shards=1."
		else:
			lat = (f"Read-side latency {a / u:.1f}x lower than `atomsnap` at shards=1." if u <= a
				else f"Read-side latency {u / a:.1f}x higher than `atomsnap` at shards=1.")
		ur = b2v(D, "A", 8, "0", "urcu", "r")
		ar = b2v(D, "A", 8, "0", "atomsnap", "r")
		if same(ur, ar):
			thr = "Reader throughput similar to `atomsnap` at CS=0us with shards=8."
		else:
			thr = (f"Reader throughput {ur / ar:.1f}x higher at CS=0us with shards=8." if ur >= ar
				else f"Reader throughput {ar / ur:.1f}x lower at CS=0us with shards=8.")
		return (f"- **urcu**: {lat} {thr} Memory usage increases with critical section length "
			f"and update rate.")

	def char_atom(D):
		k = [b2v(D, "A", s, "100", "atomsnap", "w") / b2v(D, "A", s, "100", "urcu", "w")
			for s in B2_SHARDS]
		if all(f"{x:.1f}" == "1.0" for x in k):
			w = "Writer throughput similar to `urcu` at CS=100us."
		elif all(x >= 1 for x in k):
			w = f"Writer throughput {rng(k, 1)}x higher at CS=100us."
		else:
			w = f"Writer throughput {rng([1 / x for x in k], 1)}x lower at CS=100us."
		return (f"- **atomsnap**: Read-side latency higher due to atomic refcount operations. "
			f"Memory usage constant regardless of critical section length or update rate. {w}")

	return [
		("b2A", r"^- `urcu` read latency:", lambda D: lat_line(D, "urcu")),
		("b2A", r"^- `atomsnap` read latency:", lambda D: lat_line(D, "atomsnap")),
		("b2A", r"^- `(urcu|atomsnap)` (and `(urcu|atomsnap)` )?read-side overhead ", overhead),
		("b2A", r"^- `urcu` reader throughput: ", lambda D: thr_shards(D, "urcu")),
		("b2A", r"^- `atomsnap` reader throughput: ", lambda D: thr_shards(D, "atomsnap")),
		("b2A", r"^- `urcu` RSS at CS=0us:", lambda D: urcu_rss(D, "0")),
		("b2A", r"^- `urcu` RSS at CS=100us:", lambda D: urcu_rss(D, "100")),
		("b2A", r"^- `atomsnap` RSS: ", atom_rss_A),
		("b2A", r"^- `urcu` RSS (increases|changes) ", urcu_rss_growth),
		("b2A", r"^- At CS=100us \(shards=1\):", lambda D: writer_cs100(D, 1)),
		("b2A", r"^- At CS=100us \(shards=8\):", lambda D: writer_cs100(D, 8)),
		("b2B", r"^- `urcu` RSS at 100k updates/s:", lambda D: urcu_rss_rate(D, "100k", "100k updates/s")),
		("b2B", r"^- `urcu` RSS at unlimited:", lambda D: urcu_rss_rate(D, "unlimited", "unlimited")),
		("b2B", r"^- `atomsnap` RSS: ", atom_rss_B),
		("b2B", r"^- `urcu` RSS varies by", urcu_varies),
		("b2B", r"^- `urcu` at 100k:", lambda D: rate_thr(D, "urcu")),
		("b2B", r"^- `atomsnap` at 100k:", lambda D: rate_thr(D, "atomsnap")),
		("b2B", r"^- `(atomsnap|urcu)` (and `(atomsnap|urcu)` )?reader throughput .* at 100k updates/s\.",
			lambda D: rate_win(D, "100k", "100k updates/s")),
		("b2B", r"^- `(atomsnap|urcu)` (and `(atomsnap|urcu)` )?reader throughput .* at unlimited updates/s\.",
			lambda D: rate_win(D, "unlimited", "unlimited updates/s")),
		("b2S", r"^- \*\*urcu\*\*:", char_urcu),
		("b2S", r"^- \*\*atomsnap\*\*:", char_atom),
	]


LINE_RULES = b1_line_rules() + b2_line_rules()

# ---------------------------------------------------------------------------
# Qualitative claims: never rewritten, only checked
# ---------------------------------------------------------------------------

def claim_checks(D):
	b1, out = D["b1"], []
	A, B, C = b1["A"], b1["B"], b1["C"]

	def up(d):
		return d["8/8"] > d["1/1"]

	def add(claim, ok, detail):
		out.append((claim, ok, detail))

	tops = {c: max(B1_BC_BACKENDS, key=lambda b: B["writer"][b][c]) for c in B1_CFGS}
	add("Benchmark 1 Characteristics: spinlock - Highest writer throughput in Experiment B",
		all(t == "spinlock" for t in tops.values()),
		"Experiment B 설정별 writer 1위: " + ", ".join(f"{c}={t}" for c, t in tops.items()))
	add("Benchmark 1 Characteristics: spinlock - Reader throughput decreases under high contention",
		not up(B["reader"]["spinlock"]),
		f"Experiment B spinlock reader 1/1 {m1(B['reader']['spinlock']['1/1'])} → 8/8 {m1(B['reader']['spinlock']['8/8'])}")
	add("Benchmark 1 Characteristics: shared_mutex - Reader throughput scales with thread count",
		up(B["reader"]["shared_mutex"]),
		f"Experiment B shared_mutex reader 1/1 {m1(B['reader']['shared_mutex']['1/1'])} → 8/8 {m1(B['reader']['shared_mutex']['8/8'])}")
	sm_w = C["16R/1W"]["writer"]["shared_mutex"]
	add("Benchmark 1 Characteristics: shared_mutex - Writer starvation occurs in read-heavy scenarios",
		sm_w < 1000, f"16R/1W shared_mutex writer = {commas(sm_w)} ops/sec (기준: 1,000 미만)")
	sp_ok = all(not up(e[rw]["shared_ptr"]) for e in (A, B) for rw in ("reader", "writer"))
	add("Benchmark 1 Characteristics: shared_ptr - Throughput decreases as thread count increases",
		sp_ok, "Experiment A, B의 shared_ptr reader/writer 1/1 → 8/8: " + ", ".join(
			f"{x}-{rw} {m1(e[rw]['shared_ptr']['1/1'])}→{m1(e[rw]['shared_ptr']['8/8'])}"
			for x, e in (("A", A), ("B", B)) for rw in ("reader", "writer")))
	detail = ", ".join(f"{x}-{rw} {m1(e[rw]['atomsnap']['1/1'])}→{m1(e[rw]['atomsnap']['8/8'])}"
		for x, e in (("A", A), ("B", B)) for rw in ("reader", "writer"))
	add("Benchmark 1 Characteristics: atomsnap - Reader and writer throughput scale with thread count",
		all(up(e[rw]["atomsnap"]) for e in (A, B) for rw in ("reader", "writer")),
		"Experiment A, B의 atomsnap 1/1 → 8/8: " + detail)
	mins = [B[rw]["atomsnap"][c] for rw in ("reader", "writer") for c in B1_CFGS] + \
		[C[c][rw]["atomsnap"] for c in C for rw in ("reader", "writer")]
	add("Benchmark 1 Characteristics: atomsnap - No starvation",
		min(mins) >= 10000, f"Experiment B, C에서 atomsnap 최저 처리량 = {commas(min(mins))} ops/sec (기준: 10,000 이상)")
	sm88 = B["writer"]["shared_mutex"]
	add("Experiment B: shared_mutex writer drops to near-zero at 8/8",
		sm88["8/8"] < 0.05 * sm88["1/1"],
		f"shared_mutex writer 1/1 {commas(sm88['1/1'])} → 8/8 {commas(sm88['8/8'])} (기준: 1/1의 5% 미만)")
	c = C["16R/1W"]["reader"]
	add("Experiment C: spinlock and shared_ptr show similar reader throughput at 16R/1W",
		abs(c["spinlock"] - c["shared_ptr"]) / max(c["spinlock"], c["shared_ptr"]) <= 0.10,
		f"spinlock {m1(c['spinlock'])}, shared_ptr {m1(c['shared_ptr'])} (기준: 차이 10% 이내)")

	a_rss_cs = {s: [b2v(D, "A", s, cs, "atomsnap", "rss") for cs in B2_A_CS_US] for s in B2_SHARDS}
	add("Benchmark 2 Experiment A: atomsnap RSS remains constant regardless of CS delay",
		all(is_constant(v) for v in a_rss_cs.values()),
		"shards별 CS 0/10/100us RSS(MB): " + "; ".join(f"shards={s}: {v}" for s, v in a_rss_cs.items()) + " (기준: 변동 5% 또는 0.5MB 이내)")
	a_rss_rate = [b2v(D, "B", s, k, "atomsnap", "rss") for s in B2_SHARDS for k in B2_B_RATES]
	add("Benchmark 2 Experiment B / Characteristics: atomsnap RSS is constant across update rates",
		is_constant(a_rss_rate), f"atomsnap RSS 범위 {rng(a_rss_rate, 1)}MB (기준: 변동 5% 또는 0.5MB 이내)")
	lat_hi = all(b2v(D, "A", s, "0", "atomsnap", "lat") > b2v(D, "A", s, "0", "urcu", "lat") for s in B2_SHARDS)
	add("Benchmark 2 Characteristics: atomsnap - Read-side latency higher",
		lat_hi, "CS=0us 읽기 지연(us) " + ", ".join(
			f"shards={s}: atomsnap {b2v(D, 'A', s, '0', 'atomsnap', 'lat'):.3f} / urcu {b2v(D, 'A', s, '0', 'urcu', 'lat'):.3f}"
			for s in B2_SHARDS))
	u_cs = all(b2v(D, "A", s, "100", "urcu", "rss") > b2v(D, "A", s, "0", "urcu", "rss") for s in B2_SHARDS)
	u_rate = all(b2v(D, "B", s, "unlimited", "urcu", "rss") > b2v(D, "B", s, "100k", "urcu", "rss") for s in B2_SHARDS)
	add("Benchmark 2 Characteristics: urcu - Memory usage increases with CS length and update rate",
		u_cs and u_rate, f"CS 0→100us 증가: {u_cs}, 100k→unlimited 증가: {u_rate}")
	for cs in ("0", "100"):
		a = [b2v(D, "A", s, cs, "atomsnap", "rss") for s in B2_SHARDS]
		u = [b2v(D, "A", s, cs, "urcu", "rss") for s in B2_SHARDS]
		add(f"(참고, README 문장 아님) CS={cs}us에서 atomsnap RSS가 urcu보다 작은가",
			max(a) < min(u), f"atomsnap {rng(a, 1)}MB / urcu {rng(u, 1)}MB")
	return out

# ---------------------------------------------------------------------------
# README rewriting
# ---------------------------------------------------------------------------

def rewrite_section(sec_lines, old, new):
	"""Return (new_lines, problems). problems is empty if safe to write."""
	t_old, t_new = render_tables(old), render_tables(new)
	out, problems, used = [], [], set()
	for kind, key, group, item in walk_section(sec_lines):
		if kind == "table":
			tid = TABLE_IDS.get(key)
			if tid is None:
				out.extend(item)
				continue
			cur, exp = normalize_table(item), normalize_table(t_old[tid])
			if len(cur) != len(exp):
				problems.append(f"table {tid}: README 표의 행 수가 예상과 다릅니다")
			else:
				for a, b in zip(cur, exp):
					if a == b:
						continue
					label = split_row(a)[0]
					if (tid, label) in KNOWN_README_ERRORS:
						continue
					problems.append(f"table {tid}: README 표 행이 예상과 다릅니다\n"
						f"    README : {a}\n    expected: {b}")
			out.extend(t_new[tid])
			continue
		line = item
		replaced = False
		for idx, (g, pat, fn) in enumerate(LINE_RULES):
			if g == group and re.match(pat, line):
				expect = fn(old)
				if line.rstrip() != expect:
					problems.append(f"line ({g}): README 문장이 예상과 다릅니다\n"
						f"    README : {line.rstrip()}\n    expected: {expect}")
				out.append(fn(new))
				used.add(idx)
				replaced = True
				break
		if not replaced:
			out.append(line)
	for idx, (g, pat, _) in enumerate(LINE_RULES):
		if idx not in used:
			problems.append(f"line ({g}): README에서 '{pat}' 문장을 찾지 못했습니다")
	return out, problems

# ---------------------------------------------------------------------------
# Running benchmarks
# ---------------------------------------------------------------------------

def sh(cmd, cwd):
	print(f"  $ {' '.join(cmd)}", flush=True)
	subprocess.run(cmd, cwd=cwd, check=True, stdout=subprocess.DEVNULL)


def build(repo):
	print("[build]", flush=True)
	sh(["make", "-B"], repo)
	for d in ("microbench/bench1/exchange", "microbench/bench1/cmp_exchange"):
		sh(["make", "-B"], os.path.join(repo, d))
	try:
		sh(["make", "-B"], os.path.join(repo, "microbench/bench2"))
	except subprocess.CalledProcessError:
		raise SystemExit("bench2 빌드 실패: liburcu가 필요합니다 (Ubuntu: sudo apt install liburcu-dev)")


def run_capture(cmd, cwd, timeout):
	# Run through an extra shell fork. On Linux, ru_maxrss (bench2's
	# peak_rss_kb) survives execve(), so a benchmark started directly from
	# this Python process would report Python's own RSS if that is larger.
	# The shell forks a fresh child for the benchmark, which starts clean.
	wrapped = ["sh", "-c", shlex.join(cmd) + "; s=$?; exit $s"]
	p = subprocess.run(wrapped, cwd=cwd, capture_output=True, text=True, timeout=timeout)
	if p.returncode != 0:
		raise RuntimeError(f"{' '.join(cmd)} failed ({p.returncode}):\n{p.stderr}{p.stdout}")
	return p.stdout


def parse_b1(out):
	w = re.search(r"Total writer throughput:\s*([\d.]+)", out)
	r = re.search(r"Total reader throughput:\s*([\d.]+)", out)
	if not (w and r):
		raise RuntimeError("unexpected bench1 output:\n" + out)
	return {"reader": float(r.group(1)), "writer": float(w.group(1))}


def parse_b2(out):
	rows = list(csv.DictReader(io.StringIO(out.strip())))
	if not rows:
		raise RuntimeError("unexpected bench2 output:\n" + out)
	r = rows[-1]
	return {k: float(r[k]) for k in ("r_ops_s", "w_ops_s", "peak_rss_kb", "lat_avg_ns", "lat_samples")}


def plan_runs(args):
	runs = []
	d1, d2 = args.b1_duration, args.b2_duration
	sel = (lambda bes: bes) if args.all_backends else (lambda bes: ["atomsnap"])
	for be in sel(B1_A_BACKENDS):
		for cfg in B1_CFGS:
			n = cfg.split("/")[0]
			runs.append(("b1", ("A", be, cfg), "microbench/bench1/exchange",
				[f"./{B1_PROGRAMS[be]}", n, n, str(d1)], d1))
	for be in sel(B1_BC_BACKENDS):
		for cfg in B1_CFGS:
			n = cfg.split("/")[0]
			runs.append(("b1", ("B", be, cfg), "microbench/bench1/cmp_exchange",
				[f"./{B1_PROGRAMS[be]}", n, n, str(d1)], d1))
		for cfg, (w, r) in B1_C_CFGS.items():
			runs.append(("b1", ("C", be, cfg), "microbench/bench1/cmp_exchange",
				[f"./{B1_PROGRAMS[be]}", str(w), str(r), str(d1)], d1))
	common = [f"--readers={B2_READERS}", f"--writers={B2_WRITERS}", f"--duration={d2}",
		f"--payload={B2_PAYLOAD}", "--reclaim=async", "--csv=1",
		f"--pin={args.pin}", f"--sample-pow2={args.sample_pow2}"]
	for be in sel(B2_BACKENDS):
		for s in B2_SHARDS:
			for cs in B2_A_CS_US:
				runs.append(("b2", ("A", be, s, cs), "microbench/bench2",
					["./bin/bench_all", f"--backend={be}", f"--shards={s}",
					 f"--cs-ns={int(cs) * 1000}", "--updates-per-sec=0"] + common, d2))
			for k, ups in B2_B_RATES.items():
				runs.append(("b2", ("B", be, s, k), "microbench/bench2",
					["./bin/bench_all", f"--backend={be}", f"--shards={s}",
					 f"--cs-ns={B2_B_CS_NS}", f"--updates-per-sec={ups}"] + common, d2))
	return runs


def execute(repo, runs, cache, cache_path, repeat, commit):
	results = []
	total = len(runs) * repeat
	eta = sum(r[4] for r in runs) * repeat
	print(f"[run] {len(runs)} configurations x {repeat} repeat(s), "
		f"about {eta // 60} min {eta % 60} s of benchmark time", flush=True)
	n = 0
	for bench, key, cwd, cmd, dur in runs:
		samples = []
		for rep in range(repeat):
			n += 1
			ck = f"v2|{commit}|{cwd}|{' '.join(cmd)}|rep{rep}"
			if ck in cache:
				samples.append(cache[ck])
				print(f"  [{n}/{total}] cached  {' '.join(cmd)}", flush=True)
				continue
			print(f"  [{n}/{total}] running {' '.join(cmd)}", flush=True)
			out = run_capture(cmd, os.path.join(repo, cwd), timeout=dur * 5 + 120)
			res = parse_b1(out) if bench == "b1" else parse_b2(out)
			cache[ck] = res
			with open(cache_path, "w") as f:
				json.dump(cache, f, indent=1)
			samples.append(res)
		med = {k: statistics.median(s[k] for s in samples) for k in samples[0]}
		results.append((bench, key, med))
	return results


def merge(old, results, rss_div):
	new = copy.deepcopy(old)
	lat_missing = False
	for bench, key, v in results:
		if bench == "b1":
			exp, be, cfg = key
			if exp in ("A", "B"):
				for rw in ("reader", "writer"):
					new["b1"][exp][rw][be][cfg] = v[rw]
			else:
				for rw in ("reader", "writer"):
					new["b1"]["C"][cfg][rw][be] = v[rw]
		else:
			exp, be, s, k = key
			if v["lat_samples"] == 0:
				lat_missing = True
			new["b2"][exp][s][k][be] = {"r": v["r_ops_s"] / 1e6, "w": v["w_ops_s"] / 1e6,
				"rss": v["peak_rss_kb"] / rss_div, "lat": v["lat_avg_ns"] / 1000}
	return new, lat_missing

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def pct(o, n):
	if o == 0:
		return "   n/a"
	return f"{(n - o) / o * 100:+6.1f}%"


def text_table(headers, rows):
	w = [max(len(str(h)), *(len(str(r[i])) for r in rows)) for i, h in enumerate(headers)]
	line = "  ".join(str(h).ljust(w[i]) for i, h in enumerate(headers))
	out = [line, "-" * len(line)]
	out += ["  ".join(str(r[i]).rjust(w[i]) if i else str(r[i]).ljust(w[i])
		for i in range(len(headers))) for r in rows]
	return out


def build_report(old, new, results, checks, env, args, commit, lat_missing):
	R = []
	R.append("=" * 72)
	R.append(f"atomsnap 벤치마크 재측정 결과  (commit {commit})")
	R.append("=" * 72)
	R.append(f"측정 환경 CPU : {env['cpu']}")
	R.append(f"README 환경   : {env['readme_cpu']}")
	if env["cpu_mismatch"]:
		R.append("  !! CPU 모델이 README의 Environment와 다릅니다. 같은 장비에서 측정해야 비교가 의미 있습니다.")
	R.append(f"옵션          : bench1 {args.b1_duration}s, bench2 {args.b2_duration}s, "
		f"repeat={args.repeat}, pin={args.pin}, sample-pow2={args.sample_pow2}, rss-div={args.rss_div}")
	R.append(f"재측정 대상   : {'모든 backend' if args.all_backends else 'atomsnap만 (baseline은 README 값 유지)'}")
	if lat_missing:
		R.append("  !! bench2 latency 샘플이 0개입니다. --sample-pow2를 1 이상으로 주세요.")
	R.append("")

	measured = {(b, k) for b, k, _ in results}
	b1_be = sorted({k[1] for b, k in measured if b == "b1"})
	b2_be = sorted({k[1] for b, k in measured if b == "b2"})

	R.append("[Benchmark 1] 처리량 (ops/sec)")
	for exp, title in (("A", "Experiment A: Consistency Only"),
			("B", "Experiment B: Consistency + Monotonicity")):
		rows = []
		for be in b1_be:
			if exp == "A" and be not in B1_A_BACKENDS:
				continue
			for rw in ("reader", "writer"):
				for cfg in B1_CFGS:
					o, n = old["b1"][exp][rw][be][cfg], new["b1"][exp][rw][be][cfg]
					rows.append([f"{be} {rw} {cfg}", commas(o), commas(n), pct(o, n)])
		R.append(f"  {title}")
		R += ["    " + l for l in text_table(["설정", "이전", "새 측정", "변화"], rows)]
		R.append("")
	rows = []
	for be in b1_be:
		for cfg in B1_C_CFGS:
			for rw in ("reader", "writer"):
				o, n = old["b1"]["C"][cfg][rw][be], new["b1"]["C"][cfg][rw][be]
				rows.append([f"{be} {rw} {cfg}", commas(o), commas(n), pct(o, n)])
	R.append("  Experiment C: Unbalanced")
	R += ["    " + l for l in text_table(["설정", "이전", "새 측정", "변화"], rows)]
	R.append("")

	fmt = {"r": ("reader M/s", 2), "w": ("writer M/s", 3), "rss": ("RSS MB", 1), "lat": ("lat us", 3)}
	for exp, keys, label in (("A", B2_A_CS_US, "CS(us)"), ("B", list(B2_B_RATES), "updates/s")):
		R.append(f"[Benchmark 2] Experiment {exp}")
		for be in b2_be:
			rows = []
			for s in B2_SHARDS:
				for k in keys:
					row = [f"shards={s} {label}={k}"]
					for m in ("r", "w", "rss", "lat"):
						o = old["b2"][exp][s][k][be][m]
						n = new["b2"][exp][s][k][be][m]
						row.append(f"{o:.{fmt[m][1]}f} → {n:.{fmt[m][1]}f}")
					rows.append(row)
			R.append(f"  {be} (이전 → 새 측정)")
			R += ["    " + l for l in text_table(["설정"] + [fmt[m][0] for m in ("r", "w", "rss", "lat")], rows)]
			R.append("")

	R.append("[README 문장 점검] 자동으로 고치지 않는 정성적 문장들")
	for claim, ok, detail in checks:
		R.append(f"  [{'OK' if ok else '확인 필요'}] {claim}")
		R.append(f"      {detail}")
	R.append("")
	R.append("[README와 벤치마크 코드가 다른 부분] (스크립트가 고치지 않음)")
	R.append("  - Output columns의 'lat_avg_ns: ... (excluding CS delay)': bench_all.cpp의 reader_loop는")
	R.append("    acquire 직전부터 release 직후까지 재므로 CS 시간이 포함됩니다 (표의 CS=10us 행 lat ≈ 9.7us).")
	R.append("  - Terminology의 'atomsnap uses multi-slot gates': bench_all.cpp는 shard마다 gate를 따로 만들고")
	R.append("    (num_extra_control_blocks = 0) slot 0만 씁니다.")
	return R

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def detect_cpu():
	try:
		with open("/proc/cpuinfo") as f:
			for line in f:
				if line.startswith("model name"):
					return line.split(":", 1)[1].strip()
	except OSError:
		pass
	return "unknown"


def main():
	here = os.path.dirname(os.path.abspath(__file__))
	ap = argparse.ArgumentParser(description=__doc__,
		formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--repo", default=os.path.dirname(here), help="atomsnap repository root")
	ap.add_argument("--all-backends", action="store_true",
		help="re-run baselines (shared_ptr, shared_mutex, spinlock, urcu) too")
	ap.add_argument("--update-readme", action="store_true", help="rewrite numbers in README.md")
	ap.add_argument("--repeat", type=int, default=1, help="runs per configuration (median is used)")
	ap.add_argument("--pin", type=int, default=0, choices=[0, 1], help="bench2 --pin")
	ap.add_argument("--sample-pow2", type=int, default=10,
		help="bench2 latency sampling: 1 of 2^K reads (0 disables latency)")
	ap.add_argument("--rss-div", type=float, default=1024.0,
		help="peak_rss_kb divisor for the RSS(MB) columns (1024 = MiB, 1000 = MB)")
	ap.add_argument("--b1-duration", type=int, default=B1_DURATION)
	ap.add_argument("--b2-duration", type=int, default=B2_DURATION)
	ap.add_argument("--quick", action="store_true",
		help="1-second runs to test the pipeline (README update is refused)")
	ap.add_argument("--no-build", action="store_true")
	ap.add_argument("--fresh", action="store_true", help="ignore cached results")
	args = ap.parse_args()
	if args.quick:
		args.b1_duration = args.b2_duration = 1

	repo = os.path.abspath(args.repo)
	readme_path = os.path.join(repo, "README.md")
	with open(readme_path, encoding="utf-8") as f:
		readme = f.read().split("\n")
	s, e = section_bounds(readme)
	sec = readme[s:e]
	old = parse_readme_data(sec)

	_, problems = rewrite_section(sec, old, old)
	if problems:
		print("README 형식 점검에서 문제가 발견되었습니다 (측정은 계속하지만 README는 고치지 않습니다):")
		for p in problems:
			print("  - " + p)

	commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=repo,
		capture_output=True, text=True).stdout.strip() or "unknown"
	dirty = subprocess.run(["git", "status", "--porcelain", "--", "atomsnap.c", "atomsnap.h"],
		cwd=repo, capture_output=True, text=True).stdout.strip()
	if dirty:
		commit += "-dirty"

	cpu = detect_cpu()
	readme_cpu = next((l.split("**CPU**:", 1)[1].strip() for l in sec if "**CPU**:" in l), "unknown")
	cpu_core = re.sub(r"\s*\(.*\)$", "", readme_cpu)
	env = {"cpu": cpu, "readme_cpu": readme_cpu, "cpu_mismatch": cpu_core not in cpu}

	if not args.no_build:
		build(repo)

	cache_path = os.path.join(here, "rerun_results.json")
	cache = {}
	if os.path.exists(cache_path) and not args.fresh:
		with open(cache_path) as f:
			cache = json.load(f)

	runs = plan_runs(args)
	results = execute(repo, runs, cache, cache_path, args.repeat, commit)
	new, lat_missing = merge(old, results, args.rss_div)
	checks = claim_checks(new)

	report = build_report(old, new, results, checks, env, args, commit, lat_missing)
	report_path = os.path.join(here, "rerun_report.txt")

	if args.update_readme:
		report.append("")
		if args.quick or args.b1_duration != B1_DURATION or args.b2_duration != B2_DURATION:
			report.append("[README] 측정 시간이 README의 Config와 달라서 README를 고치지 않았습니다.")
		elif problems:
			report.append("[README] 형식 점검 문제로 README를 고치지 않았습니다:")
			report += ["  - " + p for p in problems]
		elif lat_missing:
			report.append("[README] latency 샘플이 없어 README를 고치지 않았습니다.")
		else:
			new_sec, probs = rewrite_section(sec, old, new)
			if probs:
				report.append("[README] 형식 점검 문제로 README를 고치지 않았습니다:")
				report += ["  - " + p for p in probs]
			else:
				shutil.copyfile(readme_path, readme_path + ".bak")
				updated = readme[:s] + new_sec + readme[e:]
				with open(readme_path, "w", encoding="utf-8") as f:
					f.write("\n".join(updated))
				diff = list(difflib.unified_diff(readme, updated, "README.md (before)",
					"README.md (after)", lineterm=""))
				diff_path = os.path.join(here, "README.diff")
				with open(diff_path, "w", encoding="utf-8") as f:
					f.write("\n".join(diff) + "\n")
				report.append(f"[README] README.md를 갱신했습니다. 백업: README.md.bak, 변경 내역: {diff_path}")
				report.append("         [확인 필요]로 표시된 문장은 자동으로 고치지 않았으니 직접 확인하세요.")

	text = "\n".join(report)
	print()
	print(text)
	with open(report_path, "w", encoding="utf-8") as f:
		f.write(text + "\n")
	print(f"\n(보고서 저장: {report_path})")


if __name__ == "__main__":
	main()
