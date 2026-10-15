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
toda funcao de build/recomp/program/extra_functions.csv (runtime) esta no mapa.

Uso:
  python tools/check_map.py [rom.gba]            # verifica
  python tools/check_map.py [rom.gba] --update   # congela o mapa atual como baseline
"""
import argparse
import json
import subprocess
import sys
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


def invariants(m):
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
    prog = ROOT / "build" / "recomp" / "program"
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom", nargs="?")
    ap.add_argument("--update", action="store_true", help="congela o mapa atual como baseline")
    ap.add_argument("--tolerance", type=float, default=2.0, help="variacao maxima do no. de funcoes (%%)")
    args = ap.parse_args()

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
