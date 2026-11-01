#!/usr/bin/env python3
# Makes the SMK-37's firmware list (site/smk37.json), for fm1buddy's Firmware tab when an SMK-37 is on the hub: the
# Firmware Hub lists the SMK-37's firmwares (thefirmwarehub.com/smk37/) but doesn't host their packages, so this finds
# them where their makers keep them, and checks each one (its sha256, size, and the model it says it's for).
#
#   tools/smk37.py [OUT]     (default site/smk37.json; the pages workflow runs it every day)
#
# Packages: genkigenki/smk37's firmware/ (most ports), zvenson/obervave's docs/firmware/smk37, fomajam's latest
# release, and M-VAVE's own stock firmware for each model. Names, makers, kinds and descriptions come from the Hub's
# SMK-37 page; a package without a card there is left out (but for fomajam and the stock ones).
import hashlib
import html
import json
import os
import re
import sys
import time
import urllib.request

HUB = 'https://thefirmwarehub.com/'
RAW = 'https://raw.githubusercontent.com/'
STOCK = 'https://yms-file-store.oss-cn-hongkong.aliyuncs.com/software/firmware/'
MODELS = {'SMK-37 Pro': 'SMK-37Pro/SMK-37%20Pro_016.fwsc', 'SMK-37 Elite': 'SMK-37Elite/SMK-37%20Elite_016.fwsc',
          'MKE-P37': 'MKE-P37/MKE-P37_016.fwsc'}
BLOCKS, BLK, KEEP, MAX_BLOCKS = 20, 0x30, 0x2F, 64


def get(url, tries=3):
  for t in range(tries):
    try:
      req = urllib.request.Request(url, headers={'User-Agent': 'fm1buddy-smk37'})
      if 'api.github.com' in url and os.environ.get('GITHUB_TOKEN'):
        req.add_header('Authorization', 'Bearer ' + os.environ['GITHUB_TOKEN'])
      with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()
    except Exception as e:
      if t == tries - 1:
        raise RuntimeError(f'{url}: {e}')
      time.sleep(2)


def product(raw):
  """who the package is for ("SMK-37 Elite_016"), as fm1buddy reads it (fm1.cpp pkgHead)"""
  def marker(k):
    return raw[k * BLK + KEEP] if (k + 1) * BLK <= len(raw) else None
  def ok(m, i):
    return 0x20 <= (m - i - 1) & 0xFF < 0x7F
  n = BLOCKS
  while n < MAX_BLOCKS and (m := marker(n)) is not None and m != 0x7D and ok(m, n):
    n += 1
  ids = n if n > BLOCKS and marker(n) == 0x7D else BLOCKS
  return ''.join(chr((m - i - 1) & 0xFF) for i in range(ids) if (m := raw[i * BLK + KEEP]) != 0x7D)


def cards():
  """the Hub's SMK-37 cards, by id: name, author, kind, what, repo"""
  page = get(HUB + 'smk37/').decode()
  out = {}
  for m in re.finditer(r'<article class="fw" id="fw-([^"]+)"([^>]*)>(.*?)</article>', page, re.S):
    attrs, body = m.group(2), m.group(3)
    a = dict(re.findall(r'data-([a-z]+)="([^"]*)"', attrs))
    what = re.search(r'<p class="what">(.*?)</p>', body, re.S)
    who = re.search(r'class="by">[^<]*<a class="who"[^>]*>(.*?)</a>', body)  # (a port's: the original's maker)
    out[m.group(1)] = {
      'name': html.unescape(a.get('name', m.group(1))),
      'author': html.unescape(who.group(1)) if who else '',
      'kind': [k for k in a.get('kind', '').split() if k != 'port'],  # ("port": a tag on the Hub's page)
      'what': html.unescape(re.sub(r'<[^>]+>', '', what.group(1))).strip() if what else '',
      'repo': a.get('repo', ''),
    }
  return out


def tree(repo):
  t = json.loads(get(f'https://api.github.com/repos/{repo}/git/trees/HEAD?recursive=1'))
  return [e['path'] for e in t['tree'] if e['path'].endswith('.fwsc')]


def split(file):
  """'vector37-0.3.2-elite.fwsc' -> ('vector37', '0.3.2')"""
  base = re.sub(r'-(elite|mke|fm1)$', '', file[:-5])
  m = re.match(r'(.+?)-(\d.*)$', base)
  return (m.group(1), m.group(2)) if m else (base, '')


def entry(url, file, meta, version, stars=0, stock=False):
  raw = get(url)
  p = product(raw)
  model = p.split('_')[0]
  if model not in MODELS:  # an FM-1 build, or something else
    return None
  e = {'name': meta['name'], 'author': meta['author'], 'version': version, 'what': meta['what'],
       'kind': meta['kind'] or ['stock'], 'pkg': url, 'file': file, 'product': p,
       'sha256': hashlib.sha256(raw).hexdigest(), 'size': len(raw), 'stars': stars,
       'beta': 'beta' in version.lower()}
  if stock:
    e['stock'] = True
  print(f'  {p:22} {file}', file=sys.stderr)
  return e


def main():
  out = sys.argv[1] if len(sys.argv) > 1 else 'site/smk37.json'
  hub = json.loads(get(HUB + 'catalog.json'))
  stars = (hub.get('smk37') or {}).get('stars', {})
  meta = cards()
  fws = []

  for model, path in MODELS.items():
    fws.append(entry(STOCK + path, path.split('/')[-1].replace('%20', ' '),
                     {'name': 'M-VAVE stock', 'author': 'M-VAVE', 'kind': ['stock'],
                      'what': f'The firmware the {model} came with: put it back to how it was.'}, '016', stock=True))

  for path in tree('genkigenki/smk37'):
    file = path.split('/')[-1]
    fid, version = split(file)
    if not path.startswith('firmware/') or file.startswith('fm1-') or fid == 'obervave' or fid not in meta:
      continue
    fws.append(entry(RAW + 'genkigenki/smk37/HEAD/' + path, file, meta[fid], version,
                     stars.get(meta[fid]['repo'], 0)))

  for path in tree('zvenson/obervave'):
    if path.startswith('docs/firmware/smk37/') and 'obervave' in meta:
      file = path.split('/')[-1]
      fws.append(entry(RAW + 'zvenson/obervave/HEAD/' + path, file, meta['obervave'],
                       re.match(r'obervave-(.+?)-smk37', file).group(1), stars.get('zvenson/obervave', 0)))

  fj = next((f for f in hub['firmwares'] if f.get('id') == 'fomajam'), {})
  kind = fj.get('kind', 'groove')
  fjMeta = {'name': fj.get('name', 'fomajam'), 'author': fj.get('author', 'konsumer'), 'what': fj.get('what', ''),
            'kind': [kind] if isinstance(kind, str) else kind or ['groove']}
  rel = json.loads(get('https://api.github.com/repos/konsumer/fomajam/releases/latest'))
  for a in rel['assets']:
    if re.match(r'fomajam-smk37.*\.fwsc$', a['name']):
      fws.append(entry(a['browser_download_url'], a['name'], fjMeta, rel['tag_name'].lstrip('v'),
                       stars.get('konsumer/fomajam', fj.get('stars', 0))))

  fws = [f for f in fws if f]
  doc = {'updated': time.strftime('%Y-%m-%d %H:%M UTC', time.gmtime()), 'firmwares': fws}
  with open(out, 'w') as f:
    json.dump(doc, f, indent=1)
  print(f'{out}: {len(fws)} packages', file=sys.stderr)


if __name__ == '__main__':
  main()
