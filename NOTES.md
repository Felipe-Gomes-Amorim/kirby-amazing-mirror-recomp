# NOTES

## Objetivo
Recompilacao estatica: traduzir o codigo ARM/Thumb da ROM para C/C++, ligar com um
runtime que emula o hardware do GBA (PPU, audio, DMA, timers, interrupcoes, BIOS)
e rodar nativo no PC. NAO e decompilacao matching.

## Estado
- [x] Pesquisar projetos/ferramentas de recomp estatica (GBA e outros) -> secao 1
- [x] Toolchain: Windows nativo (Python 3.13, capstone, unicorn, zig cc/GCC)
- [x] Analise da ROM: ARM x Thumb, codigo x dados, mapa com ~5,65 mil funcoes
  (`tools/analyze_rom.py`), saltos indiretos classificados, confianca por funcao
- [x] Recompilador gera C compilavel e roda: crt0 -> main -> 1a VBlankIntrWait (`recomp/`)
- [x] Modelo de hardware inicial: I/O, VBlank/VCOUNT, VRAM/paleta/OAM, saida PPM (`hw/`)
- [ ] Ligar `hw/` ao runtime e passar da 1a VBlankIntrWait (IRQ real, laco de frames)
- [ ] PPU de verdade (BG tiled + sprites) e janela/entrada (SDL2)
- [ ] Som (M4A via DMA/timers), save SRAM
- [ ] Interpretador de fallback para codigo em RAM nao previsto

## Fatos da ROM (verificados nesta maquina)
- Arquivo local: `baserom.gba` (renomeado do nome original "Kirby & The Amazing Mirror
  (USA).gba"). `*.gba` esta no .gitignore.
- 16 MiB, SHA-1 `274b102b6d940f46861a92b4e65f89a51815c12c`.
- Cabecalho: titulo `AGB KIRBY AM`, game code `B8KE`, maker `01` (Nintendo), versao 0.
- Entry point: word em 0x08000000 = `ea00002e` = `B 0x080000C0` (crt0).
- Save: string `SRAM_V113` em 0x0D608A0 -> biblioteca oficial de SRAM (32 KB), nao Flash/EEPROM.
- Som: engine M4A/"Sappy" (magic `Smsh` em 0x14F458 etc.). O mixer roda em IRQ, e
  esse codigo e copiado para IWRAM (ver riscos).

## 1. Projetos e ferramentas existentes (pesquisa)
Projetos de recomp estatica de GBA (todos jovens/experimentais; verifique licenca e
estado antes de reaproveitar codigo):
- JRickey/gba-recomp (Rust, ARMv4T/Thumb -> C11, MIT/Apache-2.0, banco `gamedb.sqlite`
  CC0 de mapas de funcoes por SHA-256 da ROM). Inclui interpretador como fallback e
  oraculo de teste, contagem de ciclos por bloco, modelo de hardware em Rust via C ABI.
  https://github.com/JRickey/gba-recomp
- mstan/gbarecomp (Python CLI + C++ sharded + runtime). Analisador segue fluxo e
  interworking ARM/Thumb; codigo nao resolvido cai em interpretador. Lista Minish Cap,
  Pokemon, Mario Kart SC etc. como "experimentais". https://github.com/mstan/gbarecomp
- sp00nznet/gbarecomp (C, gcc/clang + runtime proprio; PR #3 trata jump tables Thumb
  tipo switch). https://github.com/sp00nznet/gbarecomp
- Asphaltian/GBARecomp (C#, precisa de TOML com simbolos/limites de funcao).
  https://github.com/Asphaltian/GBARecomp
- N64Recomp (MIPS -> C, base de Zelda64Recomp/DK64; renderizador RT64). Nao serve para
  ARM, mas e a referencia conceitual: lista de funcoes em TOML + runtime + "overlays".
  https://github.com/N64Recomp/N64Recomp
- Kirby Amazing Mirror: NAO achei decompilacao (pret-style) nem mapa de simbolos publico.
  Logo, o mapa de funcoes tem que ser gerado por nos (analise automatica).

Ferramentas reaproveitaveis (a instalar so depois do OK):
| Necessidade | Opcao | Uso |
|---|---|---|
| Emulador/oraculo | mGBA (MPL-2.0) | referencia de comportamento, debugger, traces para comparar |
| Disassembler | Capstone (Python) ou Ghidra (+ loader GBA) | listagem, validar nosso decoder |
| Toolchain ARM | devkitARM / arm-none-eabi-binutils | `objdump -D -b binary -m armv4t` p/ conferir |
| BIOS | NAO tem BIOS no repo; recomendado HLE das SWI | evita depender de BIOS proprietaria |
| Runtime/video/audio | SDL2 | janela, entrada, audio |
| Build | CMake + clang (ou MSVC) | compilar o C gerado |

O que da para reusar: (a) ideias/arquitetura (todos acima); (b) codigo MIT/Apache do
gba-recomp se a licenca for conferida; (c) mGBA como oraculo de teste. Nao reuse
codigo sem checar licenca e sem entender; este e um projeto de aprendizado.

## 2. Mini-curso ARM7TDMI / Thumb (com a ROM)
### Modelo
- 16 registradores de 32 bits: r0-r12, r13=SP, r14=LR, r15=PC. CPSR com flags N Z C V,
  bit T (0=ARM, 1=Thumb) e bits de modo (IRQ=0x12, System=0x1F...).
- Dois conjuntos de instrucao: ARM (32 bits, alinhado a 4, condicional em quase tudo) e
  Thumb (16 bits, alinhado a 2, poucos registradores "baixos" r0-r7, mais denso).
  Jogos usam Thumb para quase tudo (ROM e barramento de 16 bits) e ARM para rotinas
  quentes na IWRAM e no crt0.
- Troca de modo: `BX Rm` -> bit0 do endereco decide (1 = Thumb). Em Thumb, enderecos
  de funcao em ponteiros tem bit0 = 1 (ex.: 0x08152A05 aponta p/ codigo em 0x08152A04).
- Pipeline: ao ler PC voce ve instrucao_atual+8 (ARM) ou +4 (Thumb). Por isso
  `ldr r0,[pc,#imm]` usa "literal pool" perto do codigo.
- Chamada: `BL` (Thumb: par de halfwords de 22 bits; ARM: 1 word). Retorno: `bx lr` ou
  `pop {..., pc}`. Argumentos em r0-r3, retorno r0 (AAPCS, aqui via agbcc/GCC antigo).

### Exemplo real 1: crt0 em 0x080000C0 (ARM)
```
080000C0  e3a00012  mov  r0,#0x12              ; modo IRQ
080000C4  e129f000  msr  CPSR_c,r0
080000C8  e59fd028  ldr  sp,[pc,#0x28]         ; = 0x03007FA0 (pilha IRQ)
080000CC  e3a0001f  mov  r0,#0x1f              ; modo System
080000D0  e129f000  msr  CPSR_c,r0
080000D4  e59fd018  ldr  sp,[pc,#0x18]         ; = 0x03007E80 (pilha usuario)
080000D8  e59f10e0  ldr  r1,[pc,#0xe0]         ; = 0x03007FFC (vetor de IRQ na IWRAM)
080000DC  e28f0018  add  r0,pc,#0x18           ; r0 = 0x08000100 (handler de IRQ)
080000E0  e5810000  str  r0,[r1]
080000E4  e59f10d8  ldr  r1,[pc,#0xd8]         ; = 0x08152A05 (main, Thumb!)
080000E8  e1a0e00f  mov  lr,pc
080000EC  e12fff11  bx   r1                    ; ARM -> Thumb: chama 0x08152A04
080000F0  eafffff2  b    0x080000C0            ; se main voltar, reinicia
```
(Decodificado a mao; confira com `objdump` / Capstone depois.) Mostra: modos, pilhas em
IWRAM, vetor de IRQ em 0x03007FFC, e o primeiro `bx` ARM->Thumb.

### Exemplo real 2: Thumb em 0x08152A04
```
b500        push {lr}
f7fe fdc1   bl   0x0815158C      ; BL = 2 halfwords; alvo = (addr+4) + sext(hi)<<12 + lo<<1
f6ad fbdf   bl   ...
f7ff f921   bl   ...
bc01        pop  {r0}
4700        bx   r0              ; retorno "interworking" via pop+bx
```
`B530 push {r4,r5,lr}` / `B081 sub sp,#4` / `4825 ldr r0,[pc,#0x94]` em 0x08152A20 sao
o prologo tipico de funcao GCC.

### Exemplo real 3: handler de IRQ em 0x08000100 (ARM)
```
e2833c02  add r3,r3,#0x200      ; r3 = 0x04000000 (a BIOS entrega) -> 0x04000200 = REG_IE
e5932000  ldr r2,[r3]           ; IE (low) | IF (high)
e0021822  and r1,r2,r2,lsr #16  ; r1 = IE & IF  (interrupcoes pendentes e habilitadas)
```
Note o "operando 2 com shift" do ARM e a condicional (`ne`, `eq` etc.) em muitas
instrucoes; em Thumb, condicoes so em branches.

### O que um recompilador precisa de ARM/Thumb
Decodificar ~15 familias: data-processing (mov/add/sub/and/cmp...), load/store
(ldr/str/ldrb/ldrh/ldm/stm/push/pop), branches (b/bl/bx/bcond), mul, swi, msr/mrs,
swp. Cada uma -> poucas linhas de C operando sobre `regs[16]` e flags.

## 3. Mapa de memoria do GBA
| Endereco | Regiao | Tamanho | Notas |
|---|---|---|---|
| 0x00000000 | BIOS | 16 KB | so executa a partir da propria BIOS; usar HLE |
| 0x02000000 | EWRAM | 256 KB | RAM lenta (bus 16 bit); dados do jogo |
| 0x03000000 | IWRAM | 32 KB | RAM rapida (32 bit); codigo ARM quente, pilhas, 0x03007FFC=vetor IRQ |
| 0x04000000 | I/O | 1 KB | registradores (DISPCNT, DMA, TIMER, KEYINPUT, IE/IF...) |
| 0x05000000 | Palette | 1 KB | BG 256 cores + OBJ 256 cores (BGR555) |
| 0x06000000 | VRAM | 96 KB | tiles/mapas/bitmap |
| 0x07000000 | OAM | 1 KB | 128 sprites |
| 0x08000000 | ROM (WS0) | ate 32 MB | espelhos em 0x0A.. e 0x0C.. (wait states) |
| 0x0E000000 | Save | 32-64 KB | aqui SRAM (SRAM_V113) |

Registradores mais importantes: DISPCNT 0x04000000, DISPSTAT 0x04000004, VCOUNT
0x04000006, BG0CNT 0x04000008, KEYINPUT 0x04000130, IE 0x04000200, IF 0x04000202,
IME 0x04000208, DMA0-3 0x040000B0.., TIMER0-3 0x04000100.., SOUNDCNT 0x04000080.

Acesso aos dados: um `ldr r0,[pc,#imm]` com literal 0x03007FFC (visto no crt0) e um
exemplo de endereco de IWRAM; 0x08152A05 e endereco de ROM com bit Thumb.

## 4. Arquitetura proposta
```
ROM.gba --> [analisador] --> funcs.toml/json (enderecos, modo ARM/Thumb, tamanho,
                              tabelas de salto, alvos indiretos)
                 |
funcs + ROM --> [lifter+emissor] --> out/*.c  (1 funcao guest = 1 funcao C)
                 |
out/*.c + [runtime C] + SDL2 --> kirby.exe
```
### Recompilador (Python no inicio)
1. Decoder ARM+Thumb proprio (validar contra Capstone/objdump).
2. Descoberta recursiva a partir do entry, vetor de IRQ, ponteiros em tabelas; separa
   codigo de dados (literal pools, jump tables, tabelas de funcao).
3. Lifter: instrucao -> operacoes C sobre `cpu_t` (regs, flags N/Z/C/V). Flags
   calculadas "preguicosamente" quando possivel; comecar simples (calcular sempre).
4. Emissor: `void f_08152A04(cpu_t* c)`, labels por bloco, `goto`; chamadas diretas
   viram chamadas C; indirect (`bx r`, `mov pc`, `ldr pc`) -> `rt_call_indirect(c, addr)`.

### Runtime (C, depois pode virar C++)
- Barramento: `read8/16/32`, `write8/16/32` por regiao, espelhos, SRAM em arquivo.
- PPU por scanline (modos 0-2 e sprites; Kirby usa BG tiled + OBJ), VBlank/HBlank/
  VCount, IRQs. Audio: M4A roda como codigo do jogo, entao basta emular DMA de som +
  FIFO A/B + timers. Entrada: teclado -> KEYINPUT.
- BIOS HLE: SWI `VBlankIntrWait`, `Halt`, `CpuSet`, `CpuFastSet`, `LZ77`, `Div`,
  `Sqrt`, `ArcTan2`, `RegisterRamReset`... implementados em C.
- Interpretador ARM/Thumb pequeno como fallback e **oraculo**: o mesmo estado, mesmos
  efeitos; da para comparar bloco a bloco com o codigo recompilado.
- Tabela `guest_addr -> funcao nativa` para indirect calls e codigo carregado em RAM.

### Formato de saida
Diretorio `out/` com `funcs_XXXX.c` fatiados (compilacao paralela), um `funcs.h`, uma
tabela `addr->ptr`, e `CMakeLists.txt`. Nada do jogo e commitado (so a ferramenta).

### Toolchain
Windows nativo: Python 3 (analisador), CMake + clang (ou MSVC), SDL2. WSL so se algo
exigir Linux. devkitARM apenas para conferir/desmontar. (Nada instalado ainda.)

## 5. Riscos
1. **Codigo vs dados**: literal pools e jump tables dentro do codigo confundem a
   varredura (gbarecomp PR #3 teve que resolver switches Thumb).
2. **Saltos indiretos** (`bx r`, ponteiros de funcao): precisam da tabela em runtime e
   do interpretador de fallback.
3. **Codigo em RAM**: rotinas copiadas p/ IWRAM (mixer M4A, handlers) nao existem em
   endereco fixo da ROM; tratar como "overlay" (como N64Recomp) ou interpretar.
4. **Timing/IRQ**: o jogo espera VBlank/Halt; recomp nativa nao tem ciclos reais. Opcao:
   executar um frame por VBlank e injetar IRQs em pontos seguros (limites de funcao/loop).
5. **BIOS**: HLE exige acertar efeitos colaterais das SWI e do estado de IRQ.
6. **Auto-modificacao/DMA** para VRAM/OAM com timing: aceitar aproximacao no inicio.
7. **Multiplayer por cabo (4 jogadores) e efeitos raros**: fora do escopo inicial.
8. **Legal**: nunca commitar ROM/BIOS/saidas geradas; so ferramenta e runtime.
9. Escopo: ~16 MB; funcoes estimadas dezenas de milhares; manter automatizado, nada
   de mapa manual.

## 6. Ordem de trabalho sugerida
1. Fixar toolchain; conferir hash do CRC/SHA da ROM em um script.
2. Decoder Thumb+ARM em Python + teste contra `objdump`/Capstone (corpus: a ROM toda).
3. Descoberta de funcoes a partir de 0x080000C0, IRQ (0x08000100), ponteiros em ROM;
   saida `funcs.json`; medir cobertura e "buracos" (bytes nao classificados).
4. Lifter de um conjunto minimo + emissor; recompilar so crt0 + main inicial.
5. Runtime minimo: memoria + I/O + DISPCNT/VRAM -> mostrar tela (Nintendo logo/Hal).
6. Interpretador de referencia; teste diferencial com mGBA (trace de registradores).
7. Crescer cobertura: DMA, timers, IRQ/VBlank, SWI HLE, entrada.
8. Som (M4A + FIFO), SRAM, depois polimento (video/audio opcional).

## Decisoes
- Toolchain: Windows nativo (sem WSL). Analisador em Python + emissor para C; runtime em C com SDL2.
- ROM local renomeada para `baserom.gba`.
- (pendente) Reusar ideias dos projetos acima; nao copiar codigo sem checar licenca.

## Aprendizados
- A ROM era `Kirby & The Amazing Mirror (USA).gba`; agora e `baserom.gba`.
- So seguir `bl` a partir do entry acha ~620 funcoes (0.35%): o jogo despacha quase tudo
  por tabelas de ponteiros (`bx r0`). Varrer a ROM por ponteiros com bit Thumb que apontam
  para `push {..,lr}` elevou para ~11.5k funcoes. E heuristica: precisa de validacao.
- `python -I` ignora o site-packages do usuario (capstone nao e achado); rodar sem `-I`.
- Entry = `B 0x080000C0`; main em Thumb 0x08152A04 chamado por `bx` do crt0.

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

Resultado atual (B8KE v0): header ok (logo, 0x96, checksum), ~607k instrucoes (294 ARM,
resto Thumb), **5651 funcoes**: 2138 `bl`, 3110 `ptr-scan`, 386 `addr-taken-leaf` (extras do
Riven, tier A/B), 6 `bx-const`, 3 `ram-code`, 5 `ram-ptr`, 1 `adr`, 2 `entry`. 146 tabelas de
salto (1539 entradas), ~7.4% da ROM e Thumb. Indiretos nao resolvidos: 12 (stubs
`_call_via_rN` e `bx` soltos; `pop {rN}; bx rN` e retorno). 0 pontos nao decodificam; 4
conflitos ARM/Thumb (bloco RAM 2, ver abaixo). Confianca: 5251 high, 372 medium, 28 low.
Opcoes: `--extra-functions CSV` (padrao build/indirect/new_functions.csv; `''` desliga),
`--confidence CSV` (padrao build/indirect/confidence.csv; coluna `confidence`).

### `bl` Thumb como salto longo (continuacoes) - `build/analysis/continuations.csv`
agbcc usa `bl` para saltar ao epilogo comum quando o `b` (+-2 KB) nao alcanca. Decisao em
`long_jump_reason` na hora do `bl` (so Thumb, `bl` incondicional): e SALTO LONGO se o alvo
fica em (inicio da funcao dona, +8 KiB] E (a) comeca com `add sp,#imm` ou `pop`, ou (b) o
fluxo do alvo nao tem NENHUM `push` proprio e retorna por `pop {..,pc}` / `pop {rN}; bx rN`
(sem `bx lr`). Nesse caso o alvo vira bloco da mesma funcao (nao e `add_func`), `lo/hi` da
funcao dona se estendem, e a linha vai para `continuations.csv` (addr_filho, addr_pai,
motivo). Tambem: extra `addr-taken-leaf` cujo endereco ja foi decodificado dentro de outra
funcao e continuacao (`addr-taken-inside-parent`), nao funcao nova.
Resultado: 33 por `bl` + 16 extras = 49 (-49 funcoes: 5700 -> 5651). Os 51 casos do
review_low do Riven passam a ter retorno, exceto 0x0814F890 (2 bytes avulsos antes de uma
funcao) e 0x0815931C (SoftReset: `svc 1; svc 0`, nao retorna). Nova coluna em
`functions.csv`: `has_return`, `continuations`. Funcoes sem retorno alcancavel: 25 (crt0,
handler de IRQ, stubs `bx rN`, entradas de RAM-code; legitimas). O "pai" e sempre a
funcao raiz (cadeias como 0x08153C7A -> 0x08153C8A aparecem todas ligadas a 0x08153184).
Limite: janela fixa de 8 KiB e so alvos a frente; epilogos antes do inicio nao sao pegos.

Saidas novas em `build/analysis/`:
- `functions.csv`: + `confidence`, `has_return`, `continuations`, `store_addr` (endereco na ROM), `exec_addr` (endereco de
  execucao na RAM) e `ram_block` (indice em `ram_code.csv`) para funcoes dentro de codigo copiado.
- `ram_code.csv`: uma linha por copia ROM->RAM (origem, destino, tamanho, quem chama).
- `map.json`: header, functions[], regions[], ram_code[], jump_tables[], unresolved_indirect[]
  (enderecos como inteiros) - para o recompilador ler sem juntar CSVs.

### Codigo copiado ROM -> RAM (ram_code.csv)
O crt0 NAO copia nada. As copias sao `CpuSet` (swi 0x0B, wrapper 0x081592F4) numa funcao do
motor de som M4A. `analyze_rom.py` acha os wrappers de SWI (`svc #n; bx lr`) e funcoes tipo
memcpy, recupera r0/r1/r2 por propagacao de constantes (`ldr =`, `and`, `rsb`, `lsl`...) e
guarda as chamadas com origem em ROM e destino em 0x02/0x03.
| bloco | origem (ROM) | destino (RAM) | bytes | chamada | conteudo |
|---|---|---|---|---|---|
| 0 | 0x0814F46C | 0x03000060 | 1024 | 0x081500A6 (func 0x08150098) | M4A "SoundMainRAM": entrada Thumb 0x03000061 e mixer em ARM (misto) |
| 1 | 0x08158F14 | 0x03006C50 | 64 | 0x08157256 (func 0x08157210) | ARM |
| 2 | 0x08158F54 | 0x030068E0 | 384 | 0x08157260 (func 0x08157210) | ARM com 2 stubs Thumb (0x08159074/88) |
Mapeamento: `exec = destino + (rom - origem)`. Branches B/BL sao relativos, entao o codigo
guardado na ROM decodifica igual ao que roda em RAM; so literais e `bx` absolutos mudam de
significado. Funcoes dentro dos blocos: 18 (`ram_block` != vazio).
Limites: so cobre copias com constantes visiveis na propria funcao; copias por laco proprio
(ldm/stm) com argumentos vindos de RAM/parametros nao aparecem. LZ77/RL para WRAM ficam
listados como `compressed` (nao ha codigo decodificavel); nenhum caiu em codigo hoje.
Limites gerais: seeds por ponteiro so ate +-64 KiB do codigo achado sem heuristica; sem
validacao dinamica (o score estrutural e do Riven).

## Comparacao de analisadores (Riven)
`tools/discover.py` (Riven) x `tools/analyze_rom.py` (Poppy), mesma ROM:

| | discover.py | analyze_rom.py |
|---|---|---|
| Funcoes | 5362 | 5285 |
| Em comum | 5221 | 5221 |
| So neste | 141 | 64 |
| Bytes de codigo | 1.232.846 | 1.242.472 (em comum 1.202.178) |
| Jump tables | nao resolve | sim (719 alvos distintos) |
| Saidas | JSON/print | functions/regions/jumptables/unresolved.csv |

- Os 64 so do Poppy sao corpos de `case` de switch (alcancados por jump table que meu
  analisador nao resolve) e callees deles. ~40 KB de codigo que eu perco.
- Os 141 so meus sao falsos positivos da varredura de ponteiros (primeira instrucao
  `adds`/`ldr`/`andeq`, ou fora das regioes de codigo do Poppy); o filtro dele e mais estrito.
- Bug meu achado na comparacao: `bls/blt/ble` casavam com `startswith('bl')` e eram
  tratados como chamadas (11487 "funcoes", ~6100 falsas). Corrigido para usar o grupo
  CALL do capstone; os 21 "ARM" meus tambem eram ruido (Poppy: so 252 bytes ARM).
- Decisao: `analyze_rom.py` e a fonte unica do mapa. Residuo comum a investigar: 4.4k
  saltos indiretos (`bx r0`/`bx r1`) e codigo ARM em IWRAM.

## Saltos indiretos (Riven) - `tools/resolve_indirect.py`
Entrada: `build/analysis/*.csv` (so leitura). Saida: `build/indirect/` (resolved, unresolved,
new_functions, other_sites). Corrige a leitura do "4,4k saltos indiretos nao resolvidos":

| Categoria | Qtd | O que e |
|---|---|---|
| `pop {rN}; bx rN` | 5110 | **retorno** de funcao (agbcc, interworking). NAO e salto indireto |
| stubs `_call_via_rN` (`bx rN` sozinho) | 10 | libgcc: chamada por ponteiro de funcao |
| veneer `adr rN,#0; bx rN` | 1 | Thumb->ARM em 0x0814F3DA -> ARM em 0x0814F3DC (novo, nao estava no mapa) |
| outros | 2 | 0x080001BC (ARM, IRQ) e 0x0814F88C (`bx ip`); revisar a mao |

- Chamadas por ponteiro: 321 `bl _call_via_rN` (285 so via r1, `_call_via_r1`=0x0815A29C).
  Padrao tipico: `ldr r1,[obj]; mov r0,obj; bl call_via_r1` -> `obj->fn(obj)`. 275 vem de
  um campo de struct em RAM, 26 de aritmetica de indice. So 1 e constante da ROM.
  **Conclusao: nao da para resolver estaticamente; o runtime precisa de tabela
  endereco-guest -> funcao nativa e todo candidato a callback tem que estar nela.**
- Funcoes folha sem `push {lr}` que o analisador principal nao pega (ex.: callback vazio
  `bx lr`, setters `ldr r1,[pc]; str r0,[r1]; bx lr`): 562 enderecos referenciados por
  ponteiro em dados (190 em tabela vizinha de outros ponteiros = alta confianca, 372
  isoladas = media). Estao em `build/indirect/new_functions.csv` (coluna `tier`).
  Pedido ao dono do analisador (Poppy): incorporar essas 563 entradas ao mapa.
- Cuidado: ponteiro-com-bit-0 apontando para o meio do codigo (1571 casos) nao e funcao;
  sao inteiros que parecem ponteiros.

## Hardware GBA (hw/) (Gwen)
Fonte: `hw/scan_io.py baserom.gba` -> `hw/io_refs.txt` (literais `ldr rN,[pc,#..]` ARM+Thumb,
varredura da ROM inteira; heuristica, contagens incluem falso positivo em dados). Enderecos
em 0x04000000+off. Nomes sao do mapa padrao do GBA; "refs" = literais que apontam la.
Offsets impares/nao alinhados (03,05,61,63,69,71..) sao acessos a byte/halfword dentro de
um registrador maior, nao registradores novos.

### 1. Mapa de I/O usado pelo Kirby
| Off | Registrador | Tam | R/W | refs | Obs |
|---|---|---|---|---|---|
| 000 | DISPCNT | 16 | RW | 1+ | modo, BG/OBJ enable (acessos tambem via 02/03) |
| 004 | DISPSTAT | 16 | RW | 39 | flags VBlank/HBlank/VCount + IRQ enables; 05 = LYC |
| 006 | VCOUNT | 16 | R | 40 | jogo faz polling; tem que avancar |
| 008-00E | BG0..BG3CNT | 16 | RW | 13/151 | 0A (BG1CNT) e o mais citado |
| 010-01E | BGxHOFS/VOFS | 16 | W | 14.. | scroll |
| 020-03E | BG2/3 PA..PD, X/Y | 16/32 | W | 7 | afins (rot/scale) |
| 040-04A | WIN0/1H/V, WININ/OUT | 16 | W | 10 | janelas |
| 050-054 | BLDCNT/BLDALPHA/BLDY | 16 | W | 9/3 | alpha blending |
| 060-0A8 | SOUND1..4, SOUNDCNT, FIFO_A/B (0A0/0A4) | 8/16/32 | RW | ~6-18 | usa PSG + FIFO (Direct Sound) |
| 0B0-0DE | DMA0..3 (SAD/DAD/CNT) | 32/16 | RW | 14/12/12/**418** | DMA3 (0D4) = memcpy geral, o mais usado |
| 100-10E | TM0..TM3 CNT_L/H | 16 | RW | 18/12/20/33 | TM3 (10C/10E) mais usado; 100/102 provavel som FIFO |
| 120-12A | SIOMULTI0.., SIOCNT, SIOMLT_SEND | 16 | RW | 42/**156**/9 | link cable/multiplayer (4 jogadores) |
| 130 | KEYINPUT | 16 | R | 8 | input (ativo em 0) |
| 132 | KEYCNT | 16 | RW | 1 | |
| 134 | RCNT | 16 | RW | 16 | modo serial |
| 140 | JOYCNT | 16 | RW | 6 | JOY bus |
| 200 | IE | 16 | RW | 106 | |
| 202 | IF | 16 | RW(w1c) | 97 | escrever 1 limpa |
| 204 | WAITCNT | 16 | RW | 3 | pode ser no-op |
| 208 | IME | 16 | RW | 89 | |
| 300 | POSTFLG/HALTCNT(301) | 8 | RW | 2 | Halt via BIOS ou direto |
Registradores sem literal proprio (ex.: MOSAIC 04C, BG2/3 afim parciais) podem ser acessados
por base+offset a partir de um registrador ja carregado; conferir no codigo recompilado.
Sem literal != nao usado: o codigo costuma carregar 0x04000000 e usar [r, #off]. Implementar
TODO o bloco 000-208; o scan so prioriza.

### 2. Ordem minima para ver uma tela
1. Memoria: EWRAM 02000000 (256K), IWRAM 03000000 (32K), PAL 05000000 (1K), VRAM 06000000
   (96K), OAM 07000000 (1K), ROM 08000000. Espelhamento do IWRAM em 03007FFx (crt0 usa 03007FFC).
2. I/O como RAM com mascaras + registradores com efeito: IF (w1c), KEYINPUT (=0x03FF).
3. DISPCNT + BGxCNT: renderizar so modo 0 (tile) BG0..BG3 + paleta 15-bit -> RGBA.
4. VRAM/PAL: tiles 4bpp/8bpp, tilemap, prioridade. Frame em buffer 240x160 -> janela (SDL).
5. VCOUNT/DISPSTAT: contar 0..227 linhas, VBlank em 160..227, flags e IRQ enable.
6. IRQ de VBlank: ver secao 3. Depois: OAM/sprites, DMA3 (ja que e o memcpy), timers, som, serial.

### 3. Regras de tempo (para o jogo nao travar)
- 1 frame = 228 linhas x 1232 ciclos (=280896 ciclos @16.78MHz); linhas 0-159 visiveis,
  160-227 VBlank. HBlank em cada linha apos 960 ciclos.
- VBlankIntrWait (SWI 05) = ClearFlag + espera IF.VBlank com IME=1; halt ate IRQ. Runtime tem
  que (a) setar IF bit0 ao entrar em VBlank se DISPSTAT.bit3=1 e IE.bit0=1, (b) escrever
  tambem o BIOS_IF em 03007FF8 (o handler do BIOS faz OR; jogos checam isso), (c) acordar o halt.
- IntrWait/Halt (SWI 02/04, ou HALTCNT=0): modelar como "avanca tempo ate o proximo evento
  habilitado" (VBlank/HBlank/VCount/timer/DMA/serial). Sem eventos habilitados = deadlock;
  logar em vez de travar.
- Polling de VCOUNT: devolver valor consistente com o tempo emulado (avancar relogio a cada
  leitura, ou rodar por linha). Sem isso loops `while(VCOUNT<160)` giram para sempre.
- Handler de IRQ: crt0 grava ponteiro em 03007FFC; despachar IRQ chamando o handler do jogo
  com o modo/registradores do GBA (ou chamar direto a funcao recompilada).
- Timers: contador 16 bits com prescaler 1/64/256/1024, overflow gera IRQ e recarrega;
  TM3 e o mais usado (contagem de tempo/aleatoriedade).
- DMA3 imediato (start=0) completa na hora; DMA especial VBlank/HBlank (start=1/2) dispara
  nesses eventos. Link/serial: responder "sem outros consoles" (SIOCNT bit7 limpa, sem IRQ).
- Interface com `recomp/runtime/`: contrato do Nasus em `recomp/runtime/gba_bus.h` (implementado, ver 4).
- Dado do Riven: o jogo nao usa Halt/IntrWait nem SoundDriver da BIOS, so VBlankIntrWait; som e
  M4A proprio (mixer via DMA/timers). Halt/HALTCNT ficam so armazenados.

### 4. Implementacao em hw/ (feita)
Arquivos: `hw/hw.h` (mesmas assinaturas do `gba_bus.h`, para hw/ compilar sozinho),
`hw/hw.c`, teste `hw/tests/test_frame.c`, rodar `hw/tests/run.ps1` (compila com o GCC do
WinGet, saidas em `build/hw/`: `frame_bg_obj.ppm`, `frame_mode3.ppm`). Resultado atual: OK.
- API: `hw_reset`, `hw_read/hw_write(addr,size)`, `hw_step(cycles)`, `hw_irq_pending`,
  `hw_set_keys`, `hw_frame_ready` (framebuffer XRGB8888 240x160; NULL se nao fechou frame).
  `hw.c` chama `gba_read*/gba_write*` (o runtime ou o teste fornece) so para o DMA.
- Tempo: 1232 ciclos/linha, HBlank em 960, 228 linhas, VBlank flag em 160..226, frame = 280896.
  Linha renderizada ao entrar em HBlank (efeitos por linha funcionam). `hw_step` aceita
  qualquer quantidade de ciclos (varre eventos).
- Implementado: DISPCNT, DISPSTAT (flags VBlank/HBlank/VCount computadas, enables 3-5, LYC),
  VCOUNT, KEYINPUT, IE/IF(w1c)/IME, paleta/VRAM/OAM com espelhos e escrita de byte, DMA0-3
  (imediato, VBlank, HBlank, repeat, inc/dec/fixo, 16/32 bit, IRQ), timers 0-3 (prescaler,
  cascata, IRQ), render: BG0-3 texto 4/8bpp com scroll/flip/tamanhos, modos 3/4/5,
  sprites regulares 1D/2D 4/8bpp com flip e prioridade, backdrop, forced blank.
- Quirks tratados: byte na paleta replica; byte na VRAM de BG replica (limite 0x10000 modos 0-2,
  0x14000 modos 3-5), em OBJ-VRAM e OAM e ignorado; escrita de byte no IF so limpa aquele byte;
  espelho 0x06018000->0x06010000; leitura de registrador so-escrita = 0.
- SO ARMAZENADO (sem efeito ainda): BGxHOFS/VOFS afim (BG2/3 PA-PD/X/Y), WIN0/1H/V, WININ/OUT,
  MOSAIC, BLDCNT/BLDALPHA/BLDY, SOUND1-4/SOUNDCNT/FIFO (sem audio), SIOxxx/RCNT/JOY (serial sem
  parceiro), KEYCNT (IRQ de teclado nao gerada), WAITCNT, POSTFLG/HALTCNT, IME alem do bit 0.
- NAO IMPLEMENTADO: sprites afins/double-size, modo 1/2 BG afim, janelas, blending, mosaico,
  OBJ semitransparente/janela, DMA especial (timing 3: FIFO de som e captura de video ficam
  armados e nunca disparam), som, sincronia de ciclos do DMA (e instantaneo), VCount IRQ ao
  mudar LYC com a linha ja igual, HBlank DMA fora de linhas 0-159 (certo) mas IRQ HBlank em 0-227.
- Cuidado para o runtime: `hw_irq_pending` ja aplica IME e IE&IF; o runtime so decide o despacho.
  O registro BIOS_IF (0x03007FF8) e IWRAM, fica a cargo do runtime ao tratar VBlankIntrWait.

### Correcao (Riven): filtro das folhas "address-taken"
Poppy avisou que 160 das 562 extras estavam longe do codigo (0x08192A38.., 0x08D4....) e que
8 nao decodificavam. Confirmado: eram dados que "parecem ponteiro" (decodificam como `ldc`,
`udf`, `movs r0,r0`...). O codigo real termina em 0x0815BD84. `resolve_indirect.py` agora:
(1) so aceita alvos antes do fim do codigo conhecido + 0x100, (2) rejeita corpo com `udf/ldc/
stc/mcr/mrc/cdp`. Resultado: **402 folhas** (112 tier A, 290 tier B), todas em 0x08000000-
0x0815BDxx. Regerado em `build/indirect/new_functions.csv`.

## Validacao do mapa (Riven) - por que NAO mGBA e o que fizemos
- Tentado: `tools/mgba_coverage.lua` (breakpoint por funcao, build de desenvolvimento do mGBA;
  `setBreakpoint` nao existe na estavel 0.10.x). Resultado: ~5,8k breakpoints deixam o
  emulador muito lento e chegou a crashar, mesmo com bot automatico. Nao existe trace/CDL
  pronto para GBA no mGBA nem no BizHawk. Abordagem descartada (arquivos mantidos:
  `tools/mgba_coverage.lua`, `tools/coverage.py`).
- Substituto: `tools/score_functions.py` -> `build/indirect/confidence.csv`. Pontua cada funcao
  por evidencia estrutural (decodifica limpo so nas regioes de codigo, tem retorno, sem
  instrucoes de coprocessador, nao e padding `movs r0,r0`, literais dentro da ROM, alcancada
  por chamada/ponteiro). Resultado em 5848 funcoes: **5247 high, 515 medium, 86 low**.
  `low` por fonte: bl 14, ptr-scan 24, addr-taken-leaf 47, entry 1 (o `B` em 0x08000000).
- A validacao dinamica de verdade vira de graca depois: o fallback do runtime (Nasus) loga
  `build/recomp/unresolved.log` com enderecos executados que nao estao na tabela.

## Censo de SWI da BIOS (Riven)
Chamadas `swi` encontradas nas regioes de codigo (cada uma e um wrapper em 0x08159xxx):
VBlankIntrWait(0x05), Div(0x06, 2x), Sqrt(0x08), ArcTan2(0x0A), CpuSet(0x0B), CpuFastSet(0x0C),
LZ77UnCompWRAM(0x11), LZ77UnCompVRAM(0x12), RLUnCompWRAM(0x14), RLUnCompVRAM(0x15),
SoundBiasChange(0x19, 2x), RegisterRamReset(0x01), SoftReset(0x00), Stop(0x03, em 0x0815280C),
0x25 (MultiBoot, so no cabo link). Nao usa Halt/IntrWait/SoundDriver*: o som e M4A proprio.
Para o runtime (HLE da BIOS) bastam ~13 rotinas. O `swi #0xDD` em 0x089D0794 e dado, ignorar.

## Revisao das 86 funcoes de baixa confianca (Riven) - `build/indirect/review_low.csv`
| Veredito | Qtd | Significado |
|---|---|---|
| keep | 24 | callback vazio (`bx lr`), valido |
| extend-end | 45 | a funcao termina antes do epilogue (ver abaixo): o mapa tem o `hi` errado |
| merge-into-parent | 6 | "funcao" que e continuacao da anterior (alcancada por BL longo) |
| garbage | 7 | dados depois do fim do codigo (0x0822F2DA, 0x08668998..., 0x08D4....) - sairao com a reingestao |
| review | 3 | 0x0814F8AC, 0x08151C54 (provavelmente ok); 0x0815A368 e `mov pc,lr` (valida; meu check de retorno nao via `mov pc,lr`) |
| ok | 1 | entry `B 0x080000C0` do cabecalho |

**Achado importante (agbcc usa `bl` como salto longo).** O `b` Thumb alcanca so +-2 KB; em
funcoes maiores o compilador salta para o epilogue com `bl`. O analisador trata o alvo como
funcao nova: 51 funcoes grandes (ex.: 0x0800E27C, 0x08037326, 0x08038010, 0x081531A8) terminam
em `hi` ANTES do epilogue (`add sp,#N; pop {r3-r5}; mov r8,r3...; pop {r0}; bx r0`), que
aparece como "funcao" separada (0x0800EC66 etc.). 23 desses alvos comecam com `add sp,#imm`.
Consequencia: o recompilador, ao tratar `bl` como chamada, emite epilogue em outra funcao C e
volta ao pai no ponto errado. Regra de deteccao sugerida: um alvo de `bl` e salto longo quando
(a) o chamador fica sem retorno alcancavel depois do `bl` e (b) o alvo comeca com `add sp,#imm`
/ `pop` sem ter feito `push` (ou a funcao anterior contigua nao tem retorno).

### Pares pai->continuacao prontos: `build/indirect/continuations.csv` (Riven)
Gerado por `tools/continuations.py` (regra: funcao Thumb sem retorno alcancavel cujo vizinho
contiguo comeca em `hi`). 51 pares, 35 funcoes-raiz distintas; 34 tem `bl` de salto longo
(coluna `bl_sites` = enderecos dos `bl` que devem virar `goto`), 16 sao folhas-lixo sobrepostas
(sem `bl`), 1 `bl` sem sitio. Colunas: child, parent, root_parent, child_source, child_bytes,
bl_sites, first_insn. Cadeias ja resolvidas ate a raiz (ex.: 0x08037326 -> 0x08037FF0 -> raiz
0x08037314). Regerar apos mudar o mapa: `score_functions.py` e depois `continuations.py`.

## Recompilador minimo (Nasus) - `recomp/`
Estado: **crt0 (0x080000C0) -> main -> primeira VBlankIntrWait roda recompilado, sem abortar**
(135 chamadas de funcao, 73x CpuSet, 1x CpuFastSet). 5695 funcoes do mapa (593.951
instrucoes) viram C; 5 nao decodificam (swi desconhecido/dados).

### Como rodar
```
.venv/Scripts/python -m recomp fuzz            # diff por instrucao (ARM e Thumb) vs Unicorn
.venv/Scripts/python -m recomp rom --count 40  # funcoes reais da ROM vs Unicorn
.venv/Scripts/python -m recomp.program         # recompila o mapa inteiro e roda o crt0
RECOMP_SLOW=1 .venv/Scripts/python -m unittest discover -s tests -t .   # tudo (boot ~5 min 1a vez)
```
`.venv` (Python 3.13 + unicorn + capstone + ziglang; `zig cc` e o compilador C). Saidas em
`build/recomp/` (nao versionado). 1a compilacao do programa ~4.5 min (16 shards em
paralelo); re-execucoes usam objetos em cache (segundos).

### Pecas
- `recomp/decode.py`: decoder ARMv4T proprio (ARM + Thumb) -> IR comum (dp, mul, ls, lsh,
  lsm, b, bx, swi, msr, mrs). Fora do subconjunto = `Unsupported` (nunca decodifica errado).
  Sem: mul longo, swp, SPSR, coprocessador, ldrd/strd (ARMv5).
- `recomp/emit.py`: IR -> C. Uma funcao guest = `void fn_XXXXXXXX(Cpu*)`; label por
  instrucao, `goto` dentro da funcao, `dispatch:` (switch em `cpu->pc`) para saltos dinamicos
  e para entrar no meio da funcao. `bl` -> chamada C direta (alvo conhecido) ou `rt_call`;
  depois confere `cpu->pc == retorno` e, se nao, despacha ate voltar (`rt_run_until`) -
  cobre `pop {pc}`, tail call e retorno nao local. Veneers `bl _call_via_rN` (= `bx rN`)
  sao reconhecidos pelo corpo (nao por endereco) e viram salto direto para rN.
- `recomp/runtime/`: `gba_bus.[ch]` (ROM/EWRAM/IWRAM/SRAM + stub fraco de hw: I/O como
  RAM, VCOUNT que avanca por leitura, DMA imediato; `hw/` sobrescreve), `gba_rt.c`
  (memoria sobre o barramento, banco de r13/r14 por modo, MSR/MRS, tabela e despacho),
  `gba_bios.c` (HLE), `rt_main.c` (`rt_boot.exe`), `gba_cpu.[ch]` (versao do harness).
- `recomp/program.py`: monta tudo a partir de `build/analysis/functions.csv`.
- Overlays (codigo copiado p/ RAM): quando o runtime cai em endereco 0x02/0x03 sem funcao,
  grava IWRAM+EWRAM; `program.iterate` descobre a funcao nesse snapshot, emite como
  `fn_0300xxxx` num shard proprio com assinatura (4 bytes) e repete. Achou 3 no boot:
  0x03000060 (173 insns), 0x03007DA0, 0x03007D58 (todas Thumb).

### HLE da BIOS (gba_bios.c) - testado em `tests/bios_test.c`
Div, Sqrt, ArcTan2, CpuSet, CpuFastSet, LZ77 (WRAM/VRAM), RL (WRAM/VRAM), SoundBiasChange,
RegisterRamReset, SoftReset, Stop, VBlankIntrWait. Testadas com valores calculados a mao;
**ArcTan2 foi escrita de memoria (polinomio da BIOS) e so conferida em eixos/45 graus**;
RegisterRamReset zera faixas de I/O aproximadas; SoftReset, IRQ (`irq_deliver`) e o laco
completo de VBlankIntrWait **nao foram exercitados** (o teste para na 1a espera).

### Validacao (o que passou)
- Fuzz por instrucao: ~420 instrucoes validas por modo x 3 estados cada, seeds 3/5/7/11/23:
  0 divergencias (trace antes de CADA instrucao: pc, r0-r14, NZCV; estado final, modo de
  saida, EWRAM/IWRAM; falha de memoria na mesma instrucao).
- 40 funcoes Thumb reais da ROM x 30 estados: 996 casos, 13.596 passos, 0 divergencias.
  (ROM so tem 2 candidatas ARM com `stmfd {..,lr}`; ARM real nao foi coberto por funcoes.)
- Oraculo = Unicorn/QEMU **ARM926 (ARMv5)**, nao ARM7TDMI. Casos em que ele diverge do
  ARM7 sao DESCARTADOS (`cpu->skip`), logo nao validados: LDR/STR/LDRH/LDRSH/STRH nao
  alinhados, STR/STM de PC (ARM7 grava +12), PC lido com shift por registrador (+12),
  `bx`/`ldm pc` com alvo nao alinhado ou interworking via pop/ldm, saltos > 0xFFFFF000.
  Tambem excluidos (INDEFINIDO no QEMU): campos SBZ != 0, LDM com wb e base na lista,
  `bx` Thumb com bits baixos, Rs=pc. MULS: C mantido (indefinido em ARMv4).
- Chamadas (`bl`) e SWI/MSR/MRS nao entram no fuzz (so no boot).

### O que falta / riscos
- Interpretador de fallback: hoje endereco fora da tabela **aborta** com pc/lr/regs e as
  ultimas 16 chamadas (so overlays estaticos via snapshot funcionam). Codigo em RAM que
  mude depois e abortado pela assinatura.
- Sem IRQ real nem hw/ (Gwen): VBlankIntrWait no modo normal depende de
  `hw_irq_pending/hw_step`; o stub nunca dispara IRQ -> deadlock detectado e abortado.
- Sem timing; sem validacao de `bl`/pilha contra um oraculo (so o boot).
- Compilacao de 600k instrucoes em C e lenta (4.5 min); depois: emitir menos C por
  instrucao (helpers) ou so o fecho alcancavel.
