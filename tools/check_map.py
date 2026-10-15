#!/usr/bin/env python3
"""Teste de regressao do mapa de funcoes (tools/analyze_rom.py).

Roda a analise em build/check/ e compara com um baseline congelado
(build/analysis/baseline.json; fica em build/, ignorado pelo git, porque deriva da ROM).

Falha (exit 1) se:
  - o numero de funcoes mudar mais que --tolerance % (padrao 2%)
  - qualquer funcao de confianca `high` no baseline cair para `low`
  - o conjunto de continuacoes (filho,pai) perder pares
  - sumirem funcoes do baseline alem da tolerancia
Invariantes absolutos (sem baseline): checksum do header ok, 0 funcoes `suspect` sem
retorno, todo pai de continuacao e funcao do mapa, todo bloco RAM tem funcoes dentro,
toda funcao de build/recomp/program/extra_functions.csv (runtime) esta no mapa,
nenhuma tabela de salto com exatamente 64 entradas sem `cmp` detectado (teto do analisador).

Uso:
  python tools/check_map.py [rom.gba]            # verifica
  python tools/check_map.py [rom.gba] --update   # congela o mapa atual como baseline
  python tools/check_map.py --clean [rom.gba]    # instalacao limpa: do zero (so a ROM) == baseline
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "build" / "analysis" / "baseline.json"
CHECK_DIR = ROOT / "build" / "check"
RANK = {"low": 0, "unscored": 1, "medium": 2, "high": 3}


def run_analysis(rom):
    cmd = [sys.executable, str(ROOT / "tools" / "analyze_rom.py"), "-o", str(CHECK_DIR)]
    if rom:
        cmd.insert(2, rom)
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"FALHOU: a analise terminou com erro\n{r.stdout}\n{r.stderr}")
    return json.loads((CHECK_DIR / "map.json").read_text(encoding="utf-8"))


def snapshot(m):
    return {
        "schema": m["schema"],
        "functions": {str(f["addr"]): {"mode": f["mode"], "confidence": f["confidence"],
                                       "source": f["source"], "has_return": f["has_return"]}
                      for f in m["functions"]},
        "continuations": sorted([c["child"], c["parent"]] for c in m["continuations"]),
        "ram_code": [[b["store"], b["exec"], b["size"]] for b in m["ram_code"]],
    }


def invariants(m, root=None):
    root = Path(root) if root else ROOT
    errs = []
    h = m["header"]
    if not h.get("header_checksum_ok"):
        errs.append("checksum do header invalido")
    funcs = {f["addr"]: f for f in m["functions"]}
    sus = [f["addr"] for f in m["functions"] if not f["has_return"] and f.get("noreturn_kind") == "suspect"]
    if sus:
        errs.append(f"{len(sus)} funcoes sem retorno e sem explicacao: {[hex(a) for a in sus[:8]]}")
    orphans = [c for c in m["continuations"] if c["parent"] not in funcs]
    if orphans:
        errs.append(f"{len(orphans)} continuacoes com pai que nao e funcao: "
                    f"{[(hex(c['child']), hex(c['parent'])) for c in orphans[:5]]}")
    capped = [t for t in m["jump_tables"] if len(t["targets"]) == 64 and not t.get("bounded")]
    if capped:  # 64 e o teto do analisador: sem `cmp` detectado a tabela pode estar truncada
        errs.append(f"{len(capped)} tabelas de salto com exatamente 64 entradas sem cmp detectado "
                    f"(possivel truncamento): {[(hex(t['site']), hex(t['table'])) for t in capped[:8]]}")
    prog = root / "build" / "recomp" / "program"
    rt_files = [prog / "extra_functions.csv"] + sorted(prog.glob("unresolved*.log"))
    missing = []
    for rt in rt_files:  # tudo que o runtime executou/pediu e o mapa nao tinha tem que estar no mapa
        if not rt.exists():
            continue
        for line in rt.read_text(encoding="utf-8", errors="replace").splitlines():
            cell = line.split(",")[0].strip()
            if not cell or cell.lower() == "addr":
                continue
            try:
                a = int(cell, 16)
            except ValueError:
                continue
            if 0x0A000000 <= a < 0x0E000000:
                a = 0x08000000 + (a & 0x01FFFFFF)
            a &= ~1
            if 0x08000000 <= a < 0x0A000000 and a not in funcs:   # RAM (0x02/0x03) = ram-alias, fora
                missing.append(a)
    if missing:
        errs.append(f"{len(set(missing))} funcoes do runtime (extra_functions.csv/unresolved*.log) fora do "
                    f"mapa: {[hex(a) for a in sorted(set(missing))[:8]]}")
    empty = [b["block"] for b in m["ram_code"]
             if not b["compressed"] and not any(f["ram_block"] == b["block"] for f in m["functions"])]
    if empty:
        errs.append(f"blocos de codigo RAM sem funcoes: {empty}")
    return errs


def compare(old, new, tol):
    errs = []
    n0, n1 = len(old["functions"]), len(new["functions"])
    pct = 100.0 * (n1 - n0) / n0
    if abs(pct) > tol:
        errs.append(f"numero de funcoes mudou {pct:+.2f}% ({n0} -> {n1}), limite {tol}%")
    gone = [a for a in old["functions"] if a not in new["functions"]]
    if len(gone) > n0 * tol / 100:
        errs.append(f"{len(gone)} funcoes do baseline sumiram (limite {int(n0 * tol / 100)}): "
                    f"{[hex(int(a)) for a in gone[:8]]}")
    drops = [a for a, f in old["functions"].items()
             if f["confidence"] == "high" and new["functions"].get(a, {}).get("confidence") == "low"]
    if drops:
        errs.append(f"{len(drops)} funcoes high -> low: {[hex(int(a)) for a in drops[:8]]}")
    lost = [tuple(p) for p in old["continuations"]]
    have = {tuple(p) for p in new["continuations"]}
    missing = [p for p in lost if p not in have]
    if missing:
        errs.append(f"{len(missing)} continuacoes perdidas: {[(hex(c), hex(p)) for c, p in missing[:8]]}")
    if old.get("ram_code") != new.get("ram_code"):
        errs.append(f"blocos RAM mudaram: {old.get('ram_code')} -> {new.get('ram_code')}")
    return errs, pct


PIPELINE = [("analise(1)", "analyze_rom.py"), ("indiretos", "resolve_indirect.py"),
            ("confianca", "score_functions.py"), ("analise(2)", "analyze_rom.py")]


def clean_install(rom, keep=False):
    """Constroi o mapa DO ZERO numa pasta temporaria (so tools/*.py + a ROM, sem build/ previo,
    sem logs do runtime) e exige que bata com o baseline: mesmo conjunto de funcoes, continuacoes,
    blocos RAM e tabelas. E a validacao que o launcher roda antes de abrir o jogo."""
    rom = Path(rom or (ROOT / "baserom.gba"))
    if not rom.exists():
        sys.exit(f"ROM nao encontrada: {rom}")
    if not BASELINE.exists():
        sys.exit("sem baseline: rode `check_map.py --update` num mapa conhecido primeiro")
    tmp = Path(tempfile.mkdtemp(prefix="kirby_clean_"))
    try:
        (tmp / "tools").mkdir()
        for f in (ROOT / "tools").glob("*.py"):
            shutil.copyfile(f, tmp / "tools" / f.name)
        seeds = ROOT / "tools" / "map_seeds.csv"
        if seeds.exists():
            shutil.copyfile(seeds, tmp / "tools" / seeds.name)
        shutil.copyfile(rom, tmp / "baserom.gba")
        times = []
        for name, script in PIPELINE:
            t0 = time.time()
            r = subprocess.run([sys.executable, str(tmp / "tools" / script), "baserom.gba"], cwd=tmp,
                               capture_output=True, text=True)
            times.append((name, time.time() - t0))
            if r.returncode != 0:
                print(f"FALHOU (instalacao limpa): {script} saiu com {r.returncode}\n{r.stdout[-600:]}\n{r.stderr[-600:]}")
                return 1
        m = json.loads((tmp / "build" / "analysis" / "map.json").read_text(encoding="utf-8"))
        new = snapshot(m)
        old = json.loads(BASELINE.read_text(encoding="utf-8"))
        errs = invariants(m, root=tmp)
        a0, a1 = set(old["functions"]), set(new["functions"])
        if a0 != a1:
            miss, extra = sorted(a0 - a1, key=int), sorted(a1 - a0, key=int)
            errs.append(f"funcoes diferentes do baseline: {len(miss)} faltam {[hex(int(a)) for a in miss[:6]]}, "
                        f"{len(extra)} a mais {[hex(int(a)) for a in extra[:6]]} "
                        f"(faltando = descoberta so em execucao: `analyze_rom.py --export-seeds`)")
        if sorted(map(tuple, old["continuations"])) != sorted(map(tuple, new["continuations"])):
            errs.append("continuacoes diferentes do baseline")
        if old.get("ram_code") != new.get("ram_code"):
            errs.append("blocos RAM diferentes do baseline")
        total = sum(t for _, t in times)
        stats = f"{len(new['functions'])} funcoes, {len(new['continuations'])} continuacoes, " \
                f"{len(new['ram_code'])} blocos RAM; tempo {total:.0f}s ({', '.join(f'{n} {t:.0f}s' for n, t in times)})"
        if errs:
            print("FALHOU (instalacao limpa):")
            for e in errs:
                print("  -", e)
            print("  ", stats)
            return 1
        print(f"OK (instalacao limpa): mapa do zero == baseline: {stats}")
        return 0
    finally:
        if keep:
            print("pasta temporaria mantida:", tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clean", action="store_true",
                    help="constroi do zero numa pasta temporaria (so a ROM + tools) e compara com o baseline")
    ap.add_argument("--keep", action="store_true", help="com --clean: nao apagar a pasta temporaria")
    ap.add_argument("rom", nargs="?")
    ap.add_argument("--update", action="store_true", help="congela o mapa atual como baseline")
    ap.add_argument("--tolerance", type=float, default=2.0, help="variacao maxima do no. de funcoes (%%)")
    args = ap.parse_args()

    if args.clean:
        return clean_install(args.rom, args.keep)
    m = run_analysis(args.rom)
    new = snapshot(m)
    errs = invariants(m)
    if errs:
        print("FALHOU (invariantes):")
        for e in errs:
            print("  -", e)
        return 1
    if args.update:
        BASELINE.parent.mkdir(parents=True, exist_ok=True)
        BASELINE.write_text(json.dumps(new, separators=(",", ":")), encoding="utf-8")
        print(f"baseline gravado: {len(new['functions'])} funcoes, "
              f"{len(new['continuations'])} continuacoes, {len(new['ram_code'])} blocos RAM")
        return 0
    if not BASELINE.exists():
        print("sem baseline: rode com --update primeiro")
        return 2
    old = json.loads(BASELINE.read_text(encoding="utf-8"))
    errs, pct = compare(old, new, args.tolerance)
    if errs:
        print("FALHOU (regressao):")
        for e in errs:
            print("  -", e)
        return 1
    print(f"OK: {len(new['functions'])} funcoes ({pct:+.2f}%), {len(new['continuations'])} continuacoes, "
          f"{len(new['ram_code'])} blocos RAM, invariantes ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
