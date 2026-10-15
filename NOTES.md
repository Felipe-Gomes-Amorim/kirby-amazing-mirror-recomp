# NOTES

## Objetivo
Recompilacao estatica: traduzir o codigo ARM/Thumb da ROM para C/C++, ligar com um
runtime que emula o hardware do GBA (PPU, audio, DMA, timers, interrupcoes, BIOS)
e rodar nativo no PC. NAO e decompilacao matching.

## Estado
- [ ] Pesquisar ferramentas/projetos de recomp estatica existentes (GBA e outros)
- [ ] Decidir toolchain (Windows nativo vs WSL)
- [ ] Disassembler/analise: separar ARM x Thumb, codigo x dados, mapa de funcoes
- [ ] Recompilador gera C compilavel de um trecho pequeno
- [ ] Runtime minimo (memoria, I/O, PPU) mostrando uma tela

## Decisoes
(preencher)

## Aprendizados
(preencher)

## Como rodar: ambiente e analise da ROM (tools/analyze_rom.py)
Ambiente (Windows, instalado via winget/pip; so o necessario):
- Python 3.13, CMake 4.x (ja existiam), `pip install capstone`.
- Compilador: `winget install BrechtSanders.WinLibs.POSIX.UCRT` (GCC 16 + Ninja + mingw32-make).
  Abra um terminal novo para o PATH atualizar. Teste: `g++ --version`, `cmake -G Ninja ...`.

Analise (le a ROM, nao altera nada; ~30 s):
```
python tools/analyze_rom.py                 # usa baserom.gba (ou o unico *.gba da pasta)
python tools/analyze_rom.py minha.gba -o build/analysis
```
Saida em `build/analysis/` (ignorada pelo git): `header.json`, `functions.csv`,
`jumptables.csv`, `regions.csv` (arm/thumb/literal_pool/jumptable/data por faixa),
`unresolved.csv` (saltos indiretos sem alvo conhecido).

Como funciona: header -> entry (`B 0x080000C0`) -> descida recursiva seguindo B/BL/BX e o
modo ARM/Thumb -> resolve `ldr rX,=addr; bx rX`, `add rX,pc,#imm` (handler de IRQ) e switch
Thumb (`add;ldr;mov pc`, tamanho pelo `cmp #n`) -> varre a ROM por ponteiros para
`push {..,lr}` perto do codigo conhecido (heuristica, origem `ptr-scan`) -> classifica bytes.

Resultado atual (B8KE v0): header ok (logo, 0x96, checksum), ~603k instrucoes (63 ARM,
resto Thumb), 5285 funcoes (2171 por `bl`, 3110 por ponteiro), 146 tabelas de salto
(1539 entradas), ~7.4% da ROM e Thumb, 5123 indiretos nao resolvidos (quase todos `bx r0`/`bx r1`
= chamadas por ponteiro de funcao), 4 pontos que nao decodificam.
Limites conhecidos: seeds por ponteiro so ate +-64 KiB do codigo achado sem heuristica;
nao ha validacao contra falsos positivos; `tools/discover.py` (outra analise) acha 11487
funcoes porque nao tem esse limite - comparar as duas e o proximo passo.
