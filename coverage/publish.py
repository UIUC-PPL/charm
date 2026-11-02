#!/usr/bin/env python3
"""Add one coverage run to the charm-coverage site checkout.

    publish.py --site <checkout of charmplusplus/charm-coverage> --out <coverage-out>
               [--run-url URL] [--keep-html N]

Writes runs/<date>-<charm sha>/ with REPORT.md and its HTML rendering, the
summary JSON and gzipped lcov exports, the result tables, and the llvm-cov
HTML view; updates runs/index.json and the site's index.html; keeps the HTML
view only for the newest N runs (default 8). Standard library only.
"""
import argparse
import gzip
import html
import json
import os
import re
import shutil
import sys

SMALL_FILES = ["REPORT.md", "meta.txt", "tiers.tsv"]
SMALL_GLOBS = ["results-", "vacuous-", "unreached-", "programs-", "dirs-"]
REPORT_FILES = ["objects.txt", "ck-convcore.txt"]


def read_meta(out):
    meta = {}
    with open(os.path.join(out, "meta.txt")) as f:
        for line in f:
            k, _, v = line.rstrip("\n").partition(": ")
            if k:
                meta[k] = v
    return meta


def parse_headline(report_md):
    """The three Headline tables -> {group: {tier: {lines, functions, regions}}}."""
    groups = {}
    group = None
    in_headline = False
    for line in report_md.splitlines():
        if line.startswith("## "):
            in_headline = line.startswith("## Headline")
            continue
        if not in_headline:
            continue
        m = re.match(r"\*\*(.+)\*\*", line)
        if m:
            group = m.group(1)
            groups[group] = {}
            continue
        if group and line.startswith("| ") and not line.startswith("| tier"):
            cells = [c.strip() for c in line.strip("|").split("|")]
            if len(cells) == 7:
                groups[group][cells[0]] = {
                    "lines": cells[2], "functions": cells[4], "regions": cells[6],
                    "lines_covered": cells[1],
                }
    return groups


def md_to_html(text, title):
    """Enough Markdown for REPORT.md: headings, tables, fenced code, lists,
    paragraphs, inline code and bold."""
    def inline(s):
        s = html.escape(s, quote=False)
        s = re.sub(r"`([^`]*)`", r"<code>\1</code>", s)
        s = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", s)
        return s

    out = []
    lines = text.splitlines()
    i = 0
    para = []

    def flush_para():
        if para:
            out.append("<p>" + " ".join(inline(p) for p in para) + "</p>")
            para.clear()

    while i < len(lines):
        line = lines[i]
        if line.startswith("```"):
            flush_para()
            i += 1
            block = []
            while i < len(lines) and not lines[i].startswith("```"):
                block.append(lines[i])
                i += 1
            out.append("<pre>" + html.escape("\n".join(block)) + "</pre>")
            i += 1
            continue
        m = re.match(r"(#{1,6}) (.*)", line)
        if m:
            flush_para()
            n = len(m.group(1))
            out.append(f"<h{n}>{inline(m.group(2))}</h{n}>")
            i += 1
            continue
        if line.startswith("|"):
            flush_para()
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                rows.append(lines[i])
                i += 1
            out.append("<table>")
            header = True
            for r in rows:
                if re.match(r"\|\s*-", r):
                    header = False
                    continue
                cells = [c.strip() for c in r.strip().strip("|").split("|")]
                tag = "th" if header else "td"
                out.append("<tr>" + "".join(f"<{tag}>{inline(c)}</{tag}>" for c in cells) + "</tr>")
            out.append("</table>")
            continue
        if re.match(r"[-*] ", line):
            flush_para()
            out.append("<ul>")
            while i < len(lines) and re.match(r"[-*] ", lines[i]):
                item = lines[i][2:]
                i += 1
                while i < len(lines) and lines[i].startswith("  ") and lines[i].strip():
                    item += " " + lines[i].strip()
                    i += 1
                out.append(f"<li>{inline(item)}</li>")
            out.append("</ul>")
            continue
        if not line.strip():
            flush_para()
        else:
            para.append(line)
        i += 1
    flush_para()
    return page(title, "\n".join(out), nav='<a href="../../index.html">All runs</a> &middot; <a href="html/index.html">Per-file HTML view</a>')


STYLE = """
body { font: 15px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", Helvetica, Arial, sans-serif;
       max-width: 72em; margin: 2em auto; padding: 0 1em; color: #222; }
h1 { font-size: 1.6em; } h2 { font-size: 1.25em; margin-top: 2em; } h3 { font-size: 1.05em; }
table { border-collapse: collapse; margin: 0.6em 0; font-size: 0.93em; }
th, td { border: 1px solid #ccc; padding: 0.2em 0.6em; text-align: left; white-space: nowrap; }
th { background: #f0f0f0; } td.num { text-align: right; }
pre { background: #f6f6f6; padding: 0.6em; overflow-x: auto; font-size: 0.9em; }
code { background: #f6f6f6; padding: 0 0.2em; }
nav { color: #666; margin-bottom: 1em; } nav a { color: #246; }
.big { font-size: 2em; font-weight: bold; margin-right: 1.2em; }
.lbl { color: #666; font-size: 0.85em; display: block; }
.muted { color: #666; font-size: 0.9em; }
"""


def page(title, body, nav=""):
    return (f"<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
            f"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            f"<title>{html.escape(title)}</title>\n<style>{STYLE}</style></head>\n<body>\n"
            f"<nav>{nav}</nav>\n{body}\n</body></html>\n")


def write_index(site, runs):
    runs = sorted(runs, key=lambda r: r["id"], reverse=True)
    latest = runs[0]
    commit = "https://github.com/charmplusplus/charm/commit/"
    tiers = [("ci", "ci"), ("ci+all", "ci+all"), ("ci+all+ex", "ci+all+ex")]
    groups = [("charm_hand", "Charm++, hand-written files"),
              ("charm_all", "Charm++, all files incl. generated"),
              ("reconverse", "reconverse")]

    def pct(r, g, t):
        return r["headline"].get(g, {}).get(t, {}).get("lines", "")

    body = ["<h1>Charm++ and reconverse coverage</h1>",
            "<p>Line coverage of <a href=\"https://github.com/charmplusplus/charm\">Charm++</a> "
            "(branch recharm) and reconverse from clang source-based coverage, measured weekly in CI over "
            "the test tiers: <b>ci</b> (the directories the regular CI runs), <b>all</b> (every other "
            "test the aggregator Makefiles reach), <b>ex</b> (the examples). Each tier is cumulative with "
            "the ones before it. How the numbers are made, and their traps, is in "
            "<a href=\"https://github.com/charmplusplus/charm/blob/recharm/coverage/README.md\">coverage/README.md</a>.</p>"]
    body.append(f"<h2>Latest: {html.escape(latest['date'])}, charm "
                f"<a href=\"{commit}{latest['charm']}\"><code>{latest['charm'][:10]}</code></a></h2>")
    body.append("<p>")
    for g, name in groups:
        body.append(f"<span class=\"big\">{pct(latest, g, 'ci+all+ex')}<span class=\"lbl\">{name}, lines, ci+all+ex</span></span>")
    body.append("</p>")
    body.append(f"<p><a href=\"runs/{latest['id']}/REPORT.html\">Full report</a>"
                + (f" &middot; <a href=\"runs/{latest['id']}/html/index.html\">Per-file HTML view</a>" if latest.get("html") else "")
                + (f" &middot; <a href=\"{latest['run_url']}\">Workflow run</a>" if latest.get("run_url") else "")
                + "</p>")

    body.append("<h2>All runs</h2>")
    body.append("<p class=\"muted\">Lines covered, percent. The per-file HTML view is kept for the newest runs only; "
                "the report, summary JSON and lcov exports are kept for every run.</p>")
    body.append("<table><tr><th rowspan=\"2\">date</th><th rowspan=\"2\">charm</th><th rowspan=\"2\">reconverse</th>"
                + "".join(f"<th colspan=\"3\">{name}</th>" for _, name in groups)
                + "<th rowspan=\"2\">files</th></tr>")
    body.append("<tr>" + "".join(f"<th>{t}</th>" for _ in groups for _, t in tiers) + "</tr>")
    for r in runs:
        cells = [f"<td><a href=\"runs/{r['id']}/REPORT.html\">{html.escape(r['date'])}</a></td>",
                 f"<td><a href=\"{commit}{r['charm']}\"><code>{r['charm'][:10]}</code></a></td>",
                 f"<td><code>{r['reconverse'][:10]}</code></td>"]
        for g, _ in groups:
            for t, _ in tiers:
                cells.append(f"<td class=\"num\">{pct(r, g, t)}</td>")
        links = [f"<a href=\"runs/{r['id']}/REPORT.md\">md</a>",
                 f"<a href=\"runs/{r['id']}/ci+all+ex-summary.json\">json</a>",
                 f"<a href=\"runs/{r['id']}/ci+all+ex.lcov.gz\">lcov</a>"]
        if r.get("html"):
            links.insert(0, f"<a href=\"runs/{r['id']}/html/index.html\">HTML</a>")
        if r.get("run_url"):
            links.append(f"<a href=\"{r['run_url']}\">run</a>")
        cells.append("<td>" + " ".join(links) + "</td>")
        body.append("<tr>" + "".join(cells) + "</tr>")
    body.append("</table>")
    body.append("<p class=\"muted\">Shapes are process counts (1 = one process, 2 = two processes through lcrun). "
                "Function percentages differ across compilers and are not compared here; see the report of each run.</p>")
    with open(os.path.join(site, "index.html"), "w") as f:
        f.write(page("Charm++ and reconverse coverage", "\n".join(body)))
    latest_dir = os.path.join(site, "latest")
    os.makedirs(latest_dir, exist_ok=True)
    with open(os.path.join(latest_dir, "index.html"), "w") as f:
        f.write(f"<!DOCTYPE html><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"0; url=../runs/{latest['id']}/REPORT.html\">"
                f"<a href=\"../runs/{latest['id']}/REPORT.html\">Latest report</a>\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--site", required=True, help="checkout of charmplusplus/charm-coverage")
    ap.add_argument("--out", required=True, help="coverage output directory (REPORT.md, report/)")
    ap.add_argument("--run-url", default="", help="URL of the workflow run that produced it")
    ap.add_argument("--keep-html", type=int, default=8, help="keep the llvm-cov HTML view for this many newest runs")
    a = ap.parse_args()

    meta = read_meta(a.out)
    with open(os.path.join(a.out, "REPORT.md")) as f:
        report_md = f.read()
    date = meta["date"].split(" ")[0]
    charm = meta["charm"]
    run_id = f"{date}-{charm[:10]}"
    headline_raw = parse_headline(report_md)
    headline = {}
    for group, key in (("Charm++, all files", "charm_all"), ("Charm++, hand-written", "charm_hand"), ("Reconverse", "reconverse")):
        for name, tiers in headline_raw.items():
            if name.startswith(group):
                headline[key] = tiers
    if "charm_hand" not in headline:
        sys.exit("publish.py: no Headline tables in REPORT.md")

    run_dir = os.path.join(a.site, "runs", run_id)
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)
    for name in os.listdir(a.out):
        p = os.path.join(a.out, name)
        if os.path.isfile(p) and (name in SMALL_FILES or any(name.startswith(g) for g in SMALL_GLOBS)):
            shutil.copy(p, run_dir)
    rep = os.path.join(a.out, "report")
    for name in sorted(os.listdir(rep)):
        p = os.path.join(rep, name)
        if name.endswith("-summary.json") or name.endswith("-report.txt") or name in REPORT_FILES:
            shutil.copy(p, run_dir)
        elif name.endswith(".lcov"):
            with open(p, "rb") as src, gzip.open(os.path.join(run_dir, name + ".gz"), "wb", compresslevel=9) as dst:
                shutil.copyfileobj(src, dst)
    html_src = os.path.join(rep, "html")
    has_html = os.path.isdir(html_src)
    if has_html:
        shutil.copytree(html_src, os.path.join(run_dir, "html"))
    with open(os.path.join(run_dir, "REPORT.html"), "w") as f:
        f.write(md_to_html(report_md, f"Coverage {date}, charm {charm[:10]}"))

    index_path = os.path.join(a.site, "runs", "index.json")
    runs = []
    if os.path.exists(index_path):
        with open(index_path) as f:
            runs = json.load(f)
    runs = [r for r in runs if r["id"] != run_id]
    runs.append({"id": run_id, "date": meta["date"], "charm": charm, "reconverse": meta.get("reconverse", ""),
                 "shapes": meta.get("shapes", ""), "compiler": meta.get("compiler", ""),
                 "run_url": a.run_url, "html": has_html, "headline": headline})
    runs.sort(key=lambda r: r["id"], reverse=True)
    for r in runs[a.keep_html:]:
        d = os.path.join(a.site, "runs", r["id"], "html")
        if os.path.isdir(d):
            shutil.rmtree(d)
        r["html"] = False
    with open(index_path, "w") as f:
        json.dump(runs, f, indent=1)
        f.write("\n")
    open(os.path.join(a.site, ".nojekyll"), "a").close()
    write_index(a.site, runs)
    print(f"published runs/{run_id} ({'with' if has_html else 'without'} HTML view); {len(runs)} runs indexed")


if __name__ == "__main__":
    main()
