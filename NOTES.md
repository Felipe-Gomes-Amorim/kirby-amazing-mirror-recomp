# NOTES

> Diario tecnico do projeto, escrito em portugues. Resumo em ingles: [NOTES.en.md](NOTES.en.md).
> Visao geral para quem chega agora: [README.md](README.md).

## Como ler este arquivo
Este arquivo e o registro de trabalho de quatro frentes paralelas; por isso ele cresceu em
camadas. Cada secao tem um dono (ver [ROLES.md](ROLES.md)) e as mais recentes tem prioridade
sobre o plano inicial (secoes 1-6), que ficou como historico.

| Se voce quer... | Leia |
|---|---|
| O estado atual e o que falta | **Estado** (logo abaixo) |
| Entender ARM/Thumb e o mapa do GBA com exemplos reais | secoes 2 e 3 |
| Como a ROM foi analisada e o que o mapa contem | "Como rodar: ambiente e analise" e as subsecoes (Poppy) |
| Problemas do compilador original (retornos, saltos longos, indiretos) | "Saltos indiretos", "Revisao das 86...", "`bl` Thumb como salto longo" |
| O hardware simulado (PPU, DMA, timers, audio) | "Hardware GBA (hw/)" (Gwen) |
| O recompilador, o runtime e como rodar o jogo | "Recompilador minimo" (Nasus) |
| Som (M4A) e save (SRAM) | "Som (M4A) e save (SRAM)" |

## Objetivo
Recompilacao estatica: traduzir o codigo ARM/Thumb da ROM para C/C++, ligar com um
runtime que emula o hardware do GBA (PPU, audio, DMA, timers, interrupcoes, BIOS)
e rodar nativo no PC. NAO e decompilacao matching.

## Estado
Resumo: o jogo **liga e roda** como C nativo: titulo, abertura e primeira fase aparecem; ha
janela, teclado, audio PCM+PSG e save. O que resta e precisao e acabamento, nao "fazer rodar".

- [x] Pesquisar projetos/ferramentas de recomp estatica (GBA e outros) -> secao 1
- [x] Toolchain: Windows nativo (Python 3.13, capstone, unicorn, zig cc/GCC, SDL2 2.32.2)
- [x] Mapa congelado (`map.json`, ~5,7 mil funcoes) + regressao `tools/check_map.py`
- [x] Recompilador roda o jogo: 8000+ quadros, 0 abortos
- [x] Hardware ligado: BG tiled/afim, sprites, janelas, blend, mosaico, DMA, timers, FIFO PCM, PSG
- [x] Titulo, abertura e 1a fase renderizam; save SRAM de 32 KiB (`baserom.sav`)
- [x] Janela e teclado: `python -m recomp.program --window [escala]` (Z=A, X=B, Enter=START)
- [x] Passa da 1a porta (rotina SRAM copiada p/ a pilha reconhecida por conteudo; confirmado pelo usuario)
- [x] Ritmo de 59,7275 quadros/s; velocidade medida ~9,7x tempo real (gargalo: PPU, nao o codigo recompilado)
- [x] Saida de audio SDL2 escrita (`hw/audio_sdl.c`); verificada so com driver "dummy"
- [ ] **Ligar `hw/audio_sdl.c` ao `kirby.exe`**: `rt_window.c` so chama `audio_push` fraco (descarta) e nao
  chama `audio_open`; hoje o jogo roda **sem som**
- [x] Som ligado ao `kirby.exe` (audio_open + audio_push por quadro; `--mute`, `--audio-raw`)
- [x] Estalo no canal esquerdo: causa achada (latencia do IRQ de VBlank + DMA lendo alem do buffer) e corrigida
  (`hw_step_irq`, relogio congelado no handler); **a confirmar de ouvido pelo usuario**
- [ ] Volume do PSG ~2x alto (razao PSG/PCM 0,47, deveria ser 0,25): ainda nao corrigido em `hw/hw.c`
- [ ] Conferir o som contra uma referencia (mGBA) - so o usuario ouviu, sem comparacao
- [x] Leitura em 0x100F0D55: origem achada (indice 0xFFFF, comportamento do original; ver secao propria)
- [x] Interpretador de fallback para codigo em RAM (`recomp/runtime/rt_interp.c`): 0 divergencias vs compilado
- [x] Construcao do zero: `tools/build_all.py` (ROM -> jogo); caminhos relativos e cache em `%LOCALAPPDATA%\KirbyRecomp\<sha1>`
- [x] `kirby.exe` sem argumentos: arrastar ROM, duplo clique, F11; interpretador puro a ~6x tempo real
- [ ] Launcher `KirbyRecomp.exe` (arrastar ROM -> progresso -> jogo) e pacote zip com zig+SDL2
- [ ] Iniciar interpretando enquanto compila (troca de DLL no mesmo processo; proposta do Nasus)
- [ ] Oraculo de programa inteiro (testes diferenciais de `bl`/pilha/IRQ)
- [ ] Precisao de timing por ciclo; multiplayer por cabo fora do escopo
- [ ] Licenca do repositorio e revisao dos trechos desmontados deste arquivo

## Fatos da ROM (verificados nesta maquina)
- Arquivo local: `baserom.gba` (renomeado do nome original "Kirby & The Amazing Mirror
  (USA).gba"). `*.gba` esta no .gitignore.
- 16 MiB, SHA-1 `274b102b6d940f46861a92b4e65f89a51815c12c`.
- Cabecalho: titulo `AGB KIRBY AM`, game code `B8KE`, maker `01` (Nintendo), versao 0.
- Entry point: word em 0x08000000 = `ea00002e` = `B 0x080000C0` (crt0).
- Save: string `SRAM_V113` em 0x08D608A0 -> biblioteca oficial de SRAM (32 KB), nao Flash/EEPROM.
- Som: engine M4A/"Sappy" (magic `Smsh` em 0x0814F458 etc.). O mixer roda em IRQ, e
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

Ferramentas consideradas (na pratica usamos Capstone, Unicorn, SDL2 e zig cc/GCC; mGBA foi
descartado como oraculo por breakpoints, ver "Validacao do mapa"):
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
exigir Linux. devkitARM apenas para conferir/desmontar.
(Atualizado: instalados Python+capstone+unicorn, zig cc/GCC (WinLibs) e SDL2; sem CMake/WSL.)

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

## 6. Ordem de trabalho sugerida (plano inicial; itens 1-5 e 7-8 feitos, ver Estado)
1. Fixar toolchain; conferir hash do CRC/SHA da ROM em um script.
2. Decoder Thumb+ARM em Python + teste contra `objdump`/Capstone (corpus: a ROM toda).
3. Descoberta de funcoes a partir de 0x080000C0, IRQ (0x08000100), ponteiros em ROM;
   saida `funcs.json`; medir cobertura e "buracos" (bytes nao classificados).
4. Lifter de um conjunto minimo + emissor; recompilar so crt0 + main inicial.
5. Runtime minimo: memoria + I/O + DISPCNT/VRAM -> mostrar tela (Nintendo logo/Hal).
6. Interpretador de referencia; teste diferencial com mGBA (trace de registradores).
   (Mudou: o teste diferencial usa Unicorn por instrucao e funcao; mGBA foi descartado.)
7. Crescer cobertura: DMA, timers, IRQ/VBlank, SWI HLE, entrada.
8. Som (M4A + FIFO), SRAM, depois polimento (video/audio opcional).

## Decisoes
- Toolchain: Windows nativo (sem WSL). Analisador em Python + emissor para C; runtime em C com SDL2.
- ROM local renomeada para `baserom.gba`.
- Reusar so ideias dos projetos acima; nenhum codigo copiado (decisao mantida).

## Aprendizados
- A ROM era `Kirby & The Amazing Mirror (USA).gba`; agora e `baserom.gba`.
- So seguir `bl` a partir do entry acha ~620 funcoes (0,35%): o jogo despacha quase tudo por
  tabelas de ponteiros. Varrer a ROM por ponteiros Thumb para `push {..,lr}` leva a ~5,7 mil.
  (Uma versao anterior deste texto dizia ~11,5k: era bug meu, `bls/blt/ble` contados como
  chamadas; ver "Comparacao de analisadores".)
- Dos ~5,1 mil `bx rN`, quase todos sao retornos (`pop {r0}; bx r0`), nao saltos.
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
resto Thumb), **5675 funcoes**: 2140 `bl`, 3110 `ptr-scan`, 385 `addr-taken-leaf` (extras do
Riven, tier A/B), 5 `ptr-table`, 16 `m4a-jump-table`, 1 `runtime`, 1 `runtime-log`, 6 `bx-const`, 3 `ram-code`, 5 `ram-ptr`, 1 `adr`, 2 `entry`. 146 tabelas de
salto (1749 entradas), ~7.4% da ROM e Thumb. Indiretos nao resolvidos: 12 (stubs
`_call_via_rN` e `bx` soltos; `pop {rN}; bx rN` e retorno). 0 pontos nao decodificam; 4
conflitos ARM/Thumb (bloco RAM 2, ver abaixo). Confianca: 5252 high, 393 medium, 30 low.
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
`functions.csv`: `has_return`, `continuations`. O "pai" e sempre a funcao raiz (cadeias como
0x08153C7A -> 0x08153C8A ficam ligadas a 0x08153184). O `tools/continuations.py` do Riven ainda
lista 2 pares (0x0814F892<-0x0814F890, 0x08159334<-0x0815931C): sao falsos positivos da regra
dele ("vizinha sem retorno"); ambos sao funcoes reais chamadas por `bl` de outro lugar.
Limite: janela fixa de 8 KiB e so alvos a frente; epilogos antes do inicio nao sao pegos.

Saidas novas em `build/analysis/`:
- `functions.csv`: + `confidence`, `has_return`, `continuations`, `store_addr` (endereco na ROM), `exec_addr` (endereco de
  execucao na RAM) e `ram_block` (indice em `ram_code.csv`) para funcoes dentro de codigo copiado.
- `ram_code.csv`: uma linha por copia ROM->RAM (origem, destino, tamanho, quem chama).
- `map.json`: header, functions[], regions[], ram_code[], jump_tables[], unresolved_indirect[]
  (enderecos como inteiros) - para o recompilador ler sem juntar CSVs.

### Funcoes extras e descoberta em execucao (`--extra-functions`)
O analisador le, nesta ordem, os CSVs de funcoes extras (flag repetivel; padrao = estes 3;
`--extra-functions ''` desliga todos). Arquivo ausente e ignorado. A primeira origem que
registra um endereco vence; o resto conta como "ja conhecida".
| arquivo | quem escreve | source no mapa |
|---|---|---|
| `build/indirect/new_functions.csv` | Riven (`resolve_indirect.py`): folhas com ponteiro em dados | `addr-taken-leaf` (+`tier`) |
| `build/indirect/m4a_handlers.csv` | Riven: handlers da tabela de saltos do M4A (0x08B58490..) | `m4a-jump-table` / `m4a-runtime` (como no CSV) |
| `build/recomp/program/extra_functions.csv` | **runtime do Nasus**: o que o jogo executou e o mapa nao tinha | `runtime` (ou a coluna `source`) |
Formato aceito: `addr,mode,source,tier` com cabecalho, ou SEM cabecalho `addr[,mode]` (como
o runtime grava: `0814F958,thumb`). Enderecos com ou sem `0x`. Regras:
- `addr-taken-leaf` que ja foi decodificado dentro de outra funcao vira continuacao (ver acima);
  as demais fontes (jump table, runtime) SEMPRE sao entradas de funcao.
- Cada funcao nova e seguida (`bl`, branches), entao callees tambem entram no mapa; por isso
  o total pode passar do numero de linhas dos CSVs (ex.: M4A: 17 linhas -> +18: o handler
  chama 0x0814F8AE, entrada do helper `ldr/ldrb` de proximo byte, achada por `bl`).
Ciclo: jogo roda -> runtime grava `extra_functions.csv` -> `python tools/analyze_rom.py` absorve
-> `python tools/check_map.py` (confere regressao; `--update` se intencional) -> recompilar.
Como o arquivo do runtime cresce com o jogo, o baseline muda quando ele muda: esperado.

### `--merge-runtime`: absorver o que o jogo executa
`python tools/analyze_rom.py --merge-runtime` (apos rodar o jogo no runtime do Nasus):
1. guarda o `map.json` anterior, regera a analise (le `build/recomp/program/extra_functions.csv`);
2. imprime e grava em `build/analysis/merge_log.csv` (uma linha por merge: data, enderecos do
   runtime, quantos eram novos, funcoes novas no total, por `source`, lista de enderecos). O
   total pode ser maior que o do runtime: callees de uma funcao nova entram por `bl`;
3. atualiza `build/analysis/baseline.json` SO se nada regrediu (`high`->`low`, continuacoes
   perdidas, blocos RAM mudando); a contagem pode crescer sem limite. Se regrediu, o baseline
   fica como esta e o relatorio diz o motivo. Sem a flag a analise nao toca no baseline.
Exemplo real: 0x0802FC14 (callback folha sem `push`, so alcancavel por ponteiro em struct):
`runtime_new=1, functions_new=1, source=runtime`, total 5669 -> 5670.
Invariante em `check_map.py`: toda funcao ROM de `extra_functions.csv` esta no mapa.

### Entradas do runtime: logs, `ram-alias` e `runtime_seen.csv`
Alem de `extra_functions.csv`, o analisador le `build/recomp/program/unresolved*.log`
(`addr,mode,lr=..,sp=..`; linhas de texto livre sao ignoradas) como `source=runtime-log`.
Normalizacao: espelhos de ROM 0x0A/0x0C -> 0x08; bit Thumb limpo. Classificacao:
- endereco em **0x02/0x03xxxxxx** (rotina copiada para pilha/EWRAM/IWRAM, ex.: ReadSram/
  VerifySram em `sp` variavel): **`ram-alias`**, NAO vira funcao do mapa. Vai para
  `build/analysis/ram_alias.csv` e `map.json: ram_aliases`; o runtime as resolve por conteudo.
- endereco de ROM sem funcao: entra como funcao (e seus callees), como qualquer extra.
- fora disso (0x0E.. SRAM, inexistente): ignorado.
`--merge-runtime` tambem acumula tudo que o runtime ja pediu em `build/analysis/runtime_seen.csv`
(addr,mode,source,first_seen,note), lido nas execucoes seguintes: o mapa nao perde uma funcao
quando o runtime apaga/reescreve seus logs (aconteceu com 0x0801870C: `unresolved.log` das 14:28
sumiu antes do merge; funcao valida, struct-init com LCG, `bx lr` em 0x08018746). A invariante
do `check_map.py` cobre `extra_functions.csv` e `unresolved*.log` (so enderecos ROM).

### Tamanho das tabelas de salto: limite do `cmp` atravessa o desvio condicional
Bug: `cmp r0,#0xD5; bls L; b default; L: lsls; ldr; mov pc,r0`. A descida linear acaba no `b`
(incondicional) e `L` e analisado depois, numa nova `walk()` sem `last_cmp`; `add_table` cortava
em 64. Correcao: ao TOMAR um desvio condicional que implica "indice dentro do limite" (`bls`,
`blo/bcc`, `ble`, `blt`), o `imm` do `cmp` e guardado em `cmp_at[alvo]` e a `walk()` que comeca
ali o herda. (`bhi/bcs/bgt/bge` tomados = fora do limite: NAO propagam, para o `default` nao
herdar o limite de outro switch.) A tabela ganha `bounded=True` (coluna em `jumptables.csv`,
campo em `map.json`). Resultado: 0x0809DA48 64->214, 0x0809A324 64->85, 0x0800B4E0 64->100,
0x08076248 64->67 (batem com `build/indirect/jumptable_cap_fix.csv`); 1539 -> 1749 entradas;
alvos distintos 719 -> 724 (5 novos); cresceram as funcoes donas 0x0809DA30 (+139 insns),
0x08075EDC (+66) e 0x0800B414 (+5); 0 orfaos. Todas as 146 tabelas agora tem `cmp`.
Invariante (`check_map.py`): nenhuma tabela com exatamente 64 entradas e `bounded=False`.
Limite: tabela SEM `cmp` visivel (limite checado em outro lugar/por dados) continua cortando
em 64 e dispara a invariante em vez de passar em silencio.

### Alvos de switch x funcoes (`orphan_switch_targets.csv`)
Para cada alvo de `jumptables.csv` o analisador checa se alguma funcao do mapa contem a
instrucao no seu fluxo (nao so `lo<=x<hi`) e se e a MESMA funcao do `site` do switch. Hoje:
719 alvos distintos em 146 tabelas, **0 orfaos, 0 em outra funcao**. Os alvos pertencem a
funcao do switch (por construcao: sao empilhados com o dono da funcao) e `lo/hi` os cobrem,
inclusive em funcoes grandes (0x08070164: 276 insns, 0x08070164..0x08070402). Colunas: site,
table, target, site_function, target_function, kind (`orphan` | `other-function`).

### Construcao do zero, estabilidade, cache e tempo (distribuicao)
**So a ROM, qualquer pasta, sem `build/` previo:** funciona. Medido numa pasta temporaria com
apenas `tools/*.py` + `baserom.gba` (analise -> indiretos -> confianca -> analise): 5675 funcoes.
Os caminhos do analisador sao relativos ao proprio `tools/` (REPO) e a ROM e argumento; extras
ausentes sao ignorados.
**Duas passadas sao necessarias e bastam (ponto fixo):** `resolve_indirect.py` e
`score_functions.py` precisam do mapa da passada 1 e a passada 2 consome o resultado deles
(`new_functions.csv` + coluna de confianca). Passada 3 e 4 com `--no-cache` geram `map.json`
byte a byte igual ao da passada 2 (SHA-1 identico). Nao da para ser passada unica sem fundir os
tres programas; a 2a passada NAO pode reaproveitar a 1a (entradas diferentes), so o cache abaixo.
**Sementes (`tools/map_seeds.csv`, versionado, so ENDERECOS, nenhum byte do jogo):** o que so a
execucao revela e a analise estatica nao deriva (hoje: 0x0801870C e 0x0802FC14). Lido primeiro.
`python tools/analyze_rom.py --export-seeds` regrava o arquivo a partir do que o runtime viu
(`source=runtime*`). Sem ele a construcao do zero tem 1 funcao a menos que o mapa jogado.
**`python tools/check_map.py --clean [rom]`:** constroi do zero numa pasta temporaria (copia
`tools/*.py` + sementes + ROM; sem logs do runtime) e exige igualdade com o baseline: MESMO
conjunto de funcoes, continuacoes e blocos RAM, mais as invariantes. E o que o launcher roda
antes de abrir o jogo. `--keep` mantem a pasta; `--update` (sem `--clean`) congela o baseline.
Se falhar com "faltando = descoberta so em execucao" -> `--export-seeds`.
`inner_entries` (ponteiros que caem dentro de funcao) saiu de `continuations`: dependia do
`new_functions.csv` (o Riven filtra na origem os sobrepostos), o que quebrava a igualdade
mapa-do-zero x mapa-incremental sem mudar nenhuma funcao.
**Cache por SHA-1:** chave = SHA-1(ROM) + SHA-1(analyze_rom.py) + SHA-1 de cada CSV de entrada
(extras, confianca, logs) + flags. Em `build/cache/<chave>/` (`--cache-dir`, `--no-cache`);
acerto copia os 11 arquivos de saida em ~0,1 s. Nao usado com `--merge-runtime`/`--export-seeds`.
Mudar o analisador ou qualquer entrada invalida a chave sozinho.
**Tempo (esta maquina, analise de 5675 funcoes / 609k instrucoes):**
| etapa | antes | agora |
|---|---|---|
| analise (descoberta recursiva + ponteiros) | ~17 s | ~10-11 s |
| extras + codigo-RAM + funcoes | ~2 s | ~1,7 s |
| saidas (CSV/JSON, regioes) | ~4 s | ~1 s |
| **analise total** | **~20,5 s** | **~12,6 s** (cache: 0,1 s) |
| `resolve_indirect.py` / `score_functions.py` (Riven) | 5 s / 6 s | igual |
| **pipeline do zero (4 etapas)** | **~53 s** | **~37 s** |
Ganhos: `disasm_lite` em vez de objetos do capstone, `runs()` por regex em C (era laco de 16 M
bytes, 2x), contagem de bytes com `bytearray.count` (era `Counter` sobre 16 MiB), `split_ops`
com caminho rapido. Resultado verificado identico ao anterior. `--jobs` NAO foi adicionado: a
descoberta compartilha estado (grafo) e e inerentemente sequencial; o que paraleliza e o
`resolve_indirect.py` e o `score_functions.py` entre si (so se o score nao ler o que o outro
escreve; e do Riven) e a compilacao (`--jobs` do build_all, ja usado).

### Funcoes sem retorno alcancavel (`noreturn.csv`, coluna `noreturn_kind`)
24 funcoes, todas explicadas (0 `suspect`):
| kind | qtd | exemplos / motivo |
|---|---|---|
| bx-stub | 9 | `_call_via_rN` (0x0815A298..0x0815A2C8) e 0x0814FCE0: `bx rN` solto, despacho |
| ram-code | 7 | entradas do bloco M4A copiado p/ IWRAM (0x0814F46C..0x0814F80A, 0x08158F54): saem por `bx` |
| tail-call | 2 | 0x0814F890 (cai em 0x0814F892), 0x0814F8AC (`b 0x0814F892`) |
| header-branch | 1 | 0x08000000: o `B` do header |
| crt0 | 1 | 0x080000C0: termina com `b` ao inicio; chama main por `bx r1` |
| irq-handler | 1 | 0x080000FC: sai por `bx r0` do vetor (despacho) |
| mode-veneer | 1 | 0x0814F3D8: `adr r2,#0; bx r2` (Thumb->ARM) |
| infinite-loop | 1 | 0x08151C54: laco principal do jogo (volta a 0x08151C58 para sempre) |
| softreset-stop | 1 | 0x0815931C: `svc 1; svc 0` (SoftReset) |
Ajuste feito: `mov ip,lr ... bx ip` (0x0814F878) passou a ser retorno (a copia de lr
sobrevive a `bl`/`stm`/`pop`).

### Esquema de `build/analysis/map.json` (congelado, `schema = "kirby-amm-map/1"`)
Enderecos sao inteiros (decimal). Mudancas incompativeis incrementam o numero do schema.
```
{ schema, rom, rom_base,
  header: {title, game_code, maker_code, version, header_checksum_ok, entry_branch_target, ...},
  functions: [ { addr, mode("arm"|"thumb"), lo, hi, insns, bytes,
                 source("entry|bl|ptr-scan|addr-taken-leaf|ptr-table|m4a-jump-table|m4a-runtime|runtime|runtime-log|bx-const|adr|ram-code|ram-ptr"),
                 tier("A"|"B"|null), confidence("high|medium|low|unscored"),
                 jumptable(bool), has_return(bool), continuations(int),
                 noreturn_kind(str|null), store_addr(int|null), exec_addr(int|null),
                 ram_block(int|null) } ],          # ordenado por addr
  regions: [ {start, end, size, kind("arm|thumb|literal_pool|jumptable|data")} ],  # [start,end)
  ram_code: [ {block, site, method, store, exec, size, compressed, mode} ],
  ram_aliases: [ {addr, mode, seen_in[]} ],        # PCs em RAM/pilha vistos pelo runtime (NAO sao funcoes)
  continuations: [ {child, parent, why} ],          # filho e parte da funcao `parent` (so `bl` longo)
  inner_entries: [ {addr, parent} ],                # ponteiro em dados que cai DENTRO de `parent` (info)
  jump_tables: [ {site, table, mode, bounded(bool), targets[]} ],  # bounded = limite veio de um cmp
  unresolved_indirect: [ {addr, insn} ] }
```
Semantica: `lo`/`hi` cobrem a funcao inclusive blocos de continuacao e literal pools
(use `regions` para saber o que e codigo). Funcao com `exec_addr` roda em RAM nesse
endereco, guardada na ROM em `store_addr`; `exec = ram_code.exec + (addr - ram_code.store)`.

### Teste de regressao: `python tools/check_map.py [rom] [--update] [--tolerance 2]`
Roda a analise em `build/check/` e compara com `build/analysis/baseline.json` (derivado da ROM,
fica em build/, fora do git). Falha (exit 1) se: no. de funcoes varia mais que a tolerancia
(2%); funcoes do baseline somem alem da tolerancia; qualquer `high` vira `low`; o conjunto de
continuacoes perde pares; os blocos RAM mudam. Invariantes: checksum do header ok, 0 `suspect`,
todo pai de continuacao e funcao, todo bloco RAM tem funcoes. Depois de uma mudanca
INTENCIONAL no analisador: conferir e rodar com `--update`.

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
### 5. PPU por scanline + janela (segunda rodada)
- `const uint32_t *hw_get_framebuffer(void)` (240x160 XRGB8888, `HW_FB_W/HW_FB_H` em hw.h): sempre
  valido; linhas ja renderizadas do quadro corrente. `hw_frame_ready()` continua sendo o aviso de quadro completo.
- Novo: BG afim para modo 1 (BG2) e modo 2 (BG2/BG3): PA/PB/PC/PD, X/Y 28 bits com ponto de
  referencia interno por linha (relatch em linha 0 e ao escrever X/Y), tamanhos 128..1024, wrap,
  mapa de 1 byte/entrada, 8bpp. Modos 0-5 de BG agora cobertos; sprites afins/double-size NAO.
- Testes (`hw/tests/run.ps1`, ambos OK): `test_frame.c` (timing/IRQ/DMA/timer) e `test_ppu.c`
  (quadro esperado montado a mao, comparado pixel a pixel): tile normal/hflip/vflip/h+v, sprites
  8x8 com flip, prioridade OBJ vs BG (igual: OBJ vence; pior: BG vence), transparencia, corte nas
  bordas (x=236 e x=-4), e BG2 afim (identidade, escala PA=2, PD=2 por linha, offset X).
  Saidas: `build/hw/ppu_scene_a.ppm`, `ppu_scene_b.ppm`.
- Achado de teste: escrita de byte na VRAM de BG replica o byte nos dois lados (quirk real); codigo
  do jogo que usa strb em tilemap/tiles precisa de halfword. Isso e comportamento, nao bug.
- Janela: `hw/window_sdl.h/.c` (`win_open/win_poll/win_keys/win_present/win_close`; Z=A X=B
  Backspace=Select Enter=Start setas A=L S=R). **Compilado e testado** com SDL2 2.32.2 (MinGW devel em
  `C:\libs\SDL2-2.32.2`): `hw/tests/run_demo.ps1` abre a janela 3x com cena de BG+sprite (setas/Z/Esc);
  rodou 120 quadros com `SDL_VIDEODRIVER=dummy` sem erro. No SDL2 o `main` precisa de `SDL_MAIN_HANDLED`
  (ou link com SDL2main). Para o runtime: link `-lSDL2`, copiar `SDL2.dll` ao lado do .exe.
  Instalar SDL2 (ex.: `winget`/MSYS2 mingw-w64-ucrt-x86_64-SDL2 ou o zip de desenvolvimento
  do libsdl.org) fica por conta do usuario; depois: `gcc ... window_sdl.c $(sdl2-config --cflags --libs)`.

### 6. Audio PCM + janelas/blend/mosaico (terceira rodada) - substitui "so armazenado/nao implementado" dos itens 3-4 para estes
**Audio (Direct Sound A/B)**
- API (hw.h): `int hw_audio_read(int16_t *lr, int frames)` e `int hw_audio_rate(void)`. Saida fixa em
  **48000 Hz**, estereo int16 intercalado (L,R), gerada dentro de `hw_step` (o Nasus so chama hw_step e
  le o buffer de vez em quando). Retorna quantos quadros copiou; anel interno de 16384 quadros (~0,34 s),
  o mais antigo e descartado se ninguem ler. Escolha de 48 kHz: retencao de ordem zero da amostra atual
  (sem filtro); 18157 Hz sai como degraus repetidos ~2,64x. Quem quiser 18157 Hz nativo: dividir por rate.
- FIFO A (0x0A0) / B (0x0A4): 32 bytes cada, escrita 8/16/32 bit empurra bytes (LSB primeiro), cheia = descarta.
  SOUNDCNT_H bit11/bit15 = reset da FIFO A/B (nao fica gravado). SOUNDCNT_X so o bit 7 e gravavel.
- Timer 0/1 overflow (selecionado por SOUNDCNT_H bit10/bit14) consome 1 amostra int8 de cada FIFO ligada a ele
  (FIFO vazia mantem a ultima); com <=16 bytes restantes, DMA1/DMA2 em modo especial (CNT_H timing=3, destino =
  FIFO_A ou FIFO_B) transfere 4 palavras (src incrementa, dest fixo, ignora CNT_L/repeat), IRQ do DMA se bit14.
- Mix: L = B (bits 13) [+ A se bit9], R = A (bit8) [+ B se bit12]; amostra*256, volume 100% (bit2/bit3=1) ou 50%;
  clip em int16. Master = SOUNDCNT_X bit7 (desligado = silencio, FIFO nao consome).
- Teste `hw/tests/test_audio.c` (OK): onda quadrada 16 +V/16 -V em EWRAM, Timer0 reload -924: 1 s deu 48000
  quadros e **568 periodos** (esperado 18157,3/32 = 567,4); amplitude A=100*256, B a 50% = 5120; canais nao
  usados em 0; master off = silencio.
- NAO feito: PSG (SOUND1-4/WAVE), SOUNDBIAS (nivel/amostragem), SOUNDCNT_L, mistura PSG*volume, interpolacao.
  O M4A do Kirby toca as notas normais via PCM, entao a FIFO ja deve dar a musica; os canais PSG ficam mudos.
**PPU**
- Janelas: WIN0/WIN1 (H/V com wrap quando x1>x2/y1>y2, x2>240 e y2>160 limitados), janela de OBJ (sprite
  modo 2), WININ/WINOUT por camada (BG0-3, OBJ, efeitos). Prioridade win0 > win1 > obj > fora.
- Blending: BLDCNT/BLDALPHA/BLDY: alpha (EVA/EVB limitados a 16), brighten, darken; escolhe as duas camadas
  mais altas visiveis por pixel (inclui backdrop); sprite semitransparente (modo 1) mistura com o alvo 2.
- Mosaico: BG (MOSAIC H/V, bit 6 do BGxCNT; vertical por cache da linha anterior, serve p/ afim) e OBJ (bit 12
  do attr0; aproximacao por coordenada de tela).
- Testes novos em `test_ppu.c` (cena C, valores a mao): janela com bordas inclusivas/exclusivas, alpha 8+8
  = 0x7B007B, brighten = 0xFF7B7B, darken = 0x840000, alvo fora do BLDCNT sem efeito, mosaico H4 V2 (64 px).
  Saidas: `build/hw/ppu_window.ppm`, `ppu_mosaic.ppm`.
- NAO testado/implementado: sprites afins e double-size (ainda ignorados), OBJ janela e sprite semitransparente
  (codigo existe, sem teste), mosaico de OBJ (sem teste), quirks de blend entre OBJ e janela, BG afim em modos 3-5
  com PA/PD, WININ/WINOUT com bit de OBJ+mosaico, VCount IRQ ao mudar LYC. Janela SDL2: aguardando o usuario.
Rodar tudo: `hw/tests/run.ps1` (test_frame, test_ppu, test_audio).

### 7. Saida de audio SDL, PSG e custo do passo (quarta rodada)
**Saida SDL2 (`hw/audio_sdl.h/.c`)** - `audio_open()` (48000 Hz = `hw_audio_rate()`, estereo, int16;
0 ok, <0 erro com mensagem em stderr, sem crash), `audio_push(lr, frames)`, `audio_queued()`,
`audio_underruns()`, `audio_dropped()`, `audio_close()`. Anel de 16384 quadros + callback SDL; fila
limitada a **200 ms** (excedente: descarta o mais antigo e conta em `audio_dropped`); sem dado = silencio
(conta em `audio_underruns`; 2-3 no inicio e normal). Uso no laco, 1x por quadro:
`n = hw_audio_read(tmp, 4096); audio_push(tmp, n);`. Compilar junto de `hw.c`: `hw/audio_sdl.c`, `-lSDL2`.
Demos: `hw/tests/run_demo.ps1` (janela; X toca uma nota PSG) e `run_demo.ps1 -Demo audio -Seconds 2`
(PCM quadrada 567 Hz na 1a metade, pulso PSG 440 Hz na 2a). Verificado so com `SDL_AUDIODRIVER=dummy`
(sem som audivel nesta maquina de teste): o fluxo roda e, com driver invalido, sai com mensagem e codigo 2.
**PSG (canais 1-4) em hw.c, mixado ao PCM**
- Canais: 1 pulso + sweep (SOUND1CNT_L/H/X = 0x60/0x62/0x64), 2 pulso (0x68/0x6C), 3 onda (0x70/0x72/0x74,
  WAVE_RAM 0x90-0x9F em 2 bancos; a CPU acessa o banco que NAO esta tocando; bit5 = 64 amostras),
  4 ruido (0x78/0x7C, LFSR 15 ou 7 bit). SOUNDCNT_L (0x80) enables/volume por lado, SOUNDCNT_H bits 0-1
  = razao PSG 25/50/100%, SOUNDCNT_X bit7 master (desligar zera os registradores PSG), bits 0-3 de leitura =
  status dos canais.
- Sequenciador de 512 Hz (32768 ciclos): length 256 Hz, sweep 128 Hz, envelope 64 Hz. Trigger = bit 15 de CNT_X
  (nao fica gravado); length reload ao escrever o campo; DAC off (envelope 0 e dir 0 / bit7 do CNT_L da onda)
  impede o canal de ligar. Frequencias: pulso 131072/(2048-f) Hz, onda 65536/(2048-f) Hz.
- Mix: nivel centrado (+-vol), x(vol_mestre+1) x razao(1/2/4) x 32 => vol 15 a 100% = +-15360 (PCM 8 bit a
  100% = 256 por unidade). Amostrado em retencao de ordem zero a 48 kHz, sem filtro/bias analogico.
- Testes `hw/tests/test_psg.c` (OK): T1 pulso 440 ciclos/s (esperado 439,8) e amplitude 15360; T2 envelope
  (vol 15 -> 9 apos 6 passos, 0 em ~234 ms); T3 length corta em ~125 ms e status desliga; T4 sweep estoura 2047 e
  desliga; T5 canal 2; T6 onda 442 ciclos/s (esperado 442,8) e banco da CPU; T7 ruido +-vol com 4759 transicoes;
  T8 master off.
- NAO feito: SOUNDBIAS/nivel analogico, mistura exata de saturacao, frequencia de sweep com `n` nao-escrito
  quando shift=0, padrao do primeiro duty-step na troca de frequencia (usa a nova na proxima volta),
  glitches de leitura de WAVE_RAM durante a reproducao, audio dos canais com troca de banco no meio do ciclo.
**Custo de `hw_step`** (`hw/tests/bench_step.c`, -O2, 600 quadros, cena com BG + 32 sprites):
| Cena | passo fixo 128 | passo por evento (novo) |
|---|---|---|
| A: PPU, sem audio | 1.35-1.39 ms/quadro | 1.33-1.35 |
| B: PPU + PCM + PSG 1-4 | 1.42-1.44 | 1.40-1.43 |
| C: forced blank (so passo/timers/DMA) | 0.030-0.033 | 0.010 |
Orcamento de um quadro = 16,7 ms: ~8% (PPU+audio). O passo em si custa 0.01 ms (0,06%); o custo e a renderizacao
por scanline (render_line), nao `hw_step`. O passo agora vai direto para o proximo evento (fim de HBlank/linha) e,
com audio ligado, ate a proxima amostra de saida (assim FIFO/PSG ficam exatos a <1 amostra); sem audio nao ha teto.
Nao ha ganho a buscar em `hw_step`; se o PPU virar gargalo no jogo real, o proximo passo e pular linhas quando
nada mudou (cache de linha), nao mexer no passo.

### 8. Estalo no canal esquerdo - causa (Gwen)
**Resumo:** nao ha assimetria entre DMA1/FIFO A e DMA2/FIFO B em `hw/`; o estalo vem de o **reinicio do DMA
do M4A cair as vezes depois de um pedido de FIFO**, o que faz o DMA ler 16 bytes **alem do fim** do buffer
PCM. Nos dois canais acontece; so e audivel no B porque o que vem depois do buffer B e uma tabela de ponteiros
(`49 F8 14 08 B9 F8 14 08 ...` = 0x0814F849, 0x0814F8B9) e o que vem depois do buffer A e o inicio do buffer B
(amostras validas). O gatilho e a **latencia do IRQ de VBlank no runtime** (varia de 0 a ~8000 ciclos).
- **Medido** (stage 1, 3500 quadros, `--audio-raw` + log temporario no hw): `m4aSoundVSync` faz, nos DOIS
  canais e no mesmo ciclo, `CNT=0x84400004` -> `CNT_H=0x0400` -> `CNT_H=0xB600`, a cada **6 quadros**
  (1685376 ciclos). Os buffers sao de 0x630 = **1584 B** cada (A em 0x030008B0, B em 0x03000EE0; B termina em
  0x03001510). Timer0 reload = **0xFBD8 = -1064** => **264 amostras/quadro = 15768 Hz** (o NOTES do Riven diz
  18157 Hz/304: o campo de frequencia do modo e 1-based, indice 5 = 15768 Hz, 1584/264 = 6 quadros exatos,
  **zero folga no buffer**). Reinicios em ~584 periodos: 310 com 396 palavras (99 pedidos), **137 com 400
  (100 pedidos: le 1600 B)**, **136 com 392 (98 pedidos: pula 16 B)**. As 137 leituras alem do fim sao
  exatamente os `49 F8` vistos no fluxo do B (src 0x03001510 = fim do buffer B), e o salto isolado de 18688
  (= 73*256) no L tem sempre a fase 0.77 do quadro, em multiplos de 6 quadros.
- **Mecanismo:** a FIFO pede 16 B quando restam <=16. Com 1584 amostras por periodo = 99 pedidos exatos, o
  numero de pedidos entre dois reinicios e `99 - e_p + e_(p+1)`, onde e=1 se o reinicio chegou DEPOIS do
  pedido que cai logo apos o VBlank. Latencia constante => 99 sempre (correto). Latencia que oscila ao redor
  do pedido => 0->1: 100 pedidos (le alem do fim = estalo no B); 1->0: 98 pedidos (16 amostras puladas).
- **O runtime causa o jitter** (`recomp/runtime/gba_bios.c`): `rt_tick` soma 24 ciclos/chamada e so avanca o
  hw a cada 1232; `rt_tick_loop` avanca **8192** de uma vez; o IRQ so e entregue depois do passo. Resultado:
  reinicio em vcount=160, line_cyc 0..1300 (distribuicao uniforme), e as vezes em 161. Sem isso o reinicio
  cairia sempre ~0 ciclos apos o VBlank (o pedido critico fica ~896 ciclos depois, margem de ~0,9 linha).
- **Experimento** (hw_step parando no IRQ e descartando o resto, so para teste): leituras alem do fim
  137 -> 14, periodos de 98 pedidos 136 -> 13, reinicios todos em vcount=160/line_cyc<100 salvo 13 em 161.
- **O que hw/ entrega:** `uint32_t hw_step_irq(uint32_t cycles)` (hw.h): como `hw_step`, mas para no instante em
  que uma interrupcao habilitada (IME&IE&IF) FICA pendente e devolve os ciclos consumidos; o chamador entrega
  o IRQ e continua com o resto. Tambem: o passo de `hw_step` agora termina exatamente no estouro de timer (antes
  o pop da FIFO podia atrasar ate 349 ciclos). Teste `hw/tests/test_m4a_sync.c` (OK): A e B pedem nos mesmos
  instantes/offsets; com latencia constante (0..5000) sempre 99 pedidos, L==R e nenhum ponteiro; com
  latencia 0/1000 alternada: 98,100,98,100... pedidos, ponteiro (73) so no L (como medido no jogo);
  `hw_step_irq` para exatamente no VBlank.
- **Pedido ao runtime** (ROLES.md): usar `hw_step_irq` em `rt_tick`, `rt_tick_loop` e `vblank_intr_wait`
  (entregar o IRQ no ciclo exato e depois gastar o resto), e nao avancar o hw enquanto roda o handler de IRQ
  (relogio congelado ate o retorno), para o reinicio cair sempre no mesmo ponto (e_p constante).
- **Descartado:** FIFO A vs B (mesma logica, `cur` repete a ultima amostra no underflow nos dois), latch de
  SAD/DAD (recarregado do registrador em 0->1 nos dois), contagem de palavras (4 por pedido, ignora CNT_L).
  O `0x8440` do `CNT=0x84400004` com o DMA ja armado nao dispara transferencia (so troca o campo de timing),
  igual ao mGBA; o `0x0400` desarma e `0xB600` rearma recarregando SAD.

### 9. Escala do PSG corrigida para 1/4 da escala do PCM (Gwen)
- Pedido do Riven (NOTES 'Analise do estalo e do volume', item 2): GBATek diz que cada FIFO cobre a escala
  cheia (+-0x200) e cada PSG um quarto (+-0x80). Antes: `nivel * (vol_mestre+1) * razao * 32`, com vol 15,
  master 7 e razao 100% => +-15360 (0,47 do PCM, +-32768); o PSG saia ~1,9x (6 dB) alto demais.
- Agora o fator e **16**: o mesmo caso da +-7680 (0,23 do PCM; 0,25 seria +-8192, o vol 15 nao e o
  fundo de escala do DAC de 4 bit). `hw.c` (`psg_mix`) e `hw/tests/test_psg.c` (7680, 9*512 no envelope,
  4608 em ~100 ms) atualizados. **PCM nao mudou**: `test_audio` igual (A = 100*256 = 25600, B a 50% = 5120,
  568 periodos); a conta do PCM (`amostra*256`, volume 50/100%) nao foi tocada.
- Escala atual por canal PSG (L ou R, antes do clip): `+-vol(0..15) * (vol_mestre 1..8) * razao(1,2,4 p/ 25,
  50, 100%) * 16`. Maximo teorico de 4 canais no mesmo lado: 4*7680 = 30720 (pode saturar junto com o PCM).
- Nao verificado de ouvido que o 'som de dano' e PSG (a hipotese e do Riven).

### 10. Janela minimizada nao trava mais (Gwen)
- Sintoma: ao minimizar, o jogo travava e o audio engasgava (underruns). Causa: `window_sdl.c` criava o
  renderer com `SDL_RENDERER_PRESENTVSYNC` e chamava `SDL_RenderPresent` todo quadro; com a janela
  minimizada o present bloqueia.
- `hw/window_sdl.c`: (1) `win_present` retorna na hora se `SDL_GetWindowFlags` tem MINIMIZED/HIDDEN
  (`win_visible()`); (2) sem PRESENTVSYNC por padrao (o relogio do audio ja paceia o jogo); opcao:
  `win_open_ex(titulo, escala, 1)` ou env `HW_WIN_VSYNC=1`; 'nearest' mantido; (3) `win_poll` trata
  MINIMIZED/HIDDEN e RESTORED/SHOWN/EXPOSED so atualizando um flag (sem bloquear); ao restaurar volta a
  apresentar sem compensar tempo (o relogio e o do audio; nada e "recuperado").
- API nova (as antigas intactas): `win_open_ex`, `win_visible`, `win_test_minimize(on)` (gancho de teste).
  O `kirby.exe` do Nasus usa `win_open/win_poll/win_present` com as mesmas assinaturas: basta recompilar.
- Teste `hw/tests/test_minimize.c` (build: `hw/tests/run_demo.ps1 -Demo minimize`): 60 s com 5 ciclos
  minimizar/restaurar (10 trocas), pacing pelo relogio do audio como o runtime. Resultado nesta maquina:
  3586 quadros (59,76 q/s; esperado 59,73), 1627 quadros com a janela minimizada, present maximo 10,8 ms
  (0,01 ms minimizada), espera pelo audio max 62 ms, **underruns apos o aquecimento = 0, descartados = 0**.
  Nao testei a janela do jogo real (so a de teste); com o runtime, o ritmo continua sendo o do audio.

### 11. Launcher/UX: soltar ROM, progresso, icone, titulo, kirby.ini (Gwen)
Plano: NOTES 'Distribuicao: arrastar a ROM (Riven)'. Tudo em `hw/window_sdl.[ch]` e `hw/config_ini.[ch]`
(sem dependencia de recomp/). Assinaturas antigas intactas.
- **Soltar arquivo:** `win_poll` captura `SDL_DROPFILE`; `const char *win_dropped_path(void)` devolve o
  caminho **uma vez** (depois NULL ate o proximo drop; o ponteiro vale ate o proximo drop). Tambem captura
  durante `win_progress`. Uso: `if ((p = win_dropped_path())) trocar_de_rom(p);`.
- **Tela de progresso** (janela propria 480x140, renderer por software, sem vsync, sem console):
  `win_open_progress(titulo)`, `int win_progress(percent, texto)` (0..100, limita fora da faixa; devolve
  **0 se o usuario fechou a janela**, para o launcher poder abortar), `win_close_progress()`. Barra simples,
  fonte bitmap 5x7 embutida (A-Z, 0-9 e `. , : - _ / % ( ) ! ? & ' + = < >`; minusculas usam maiusculas;
  desconhecido vira `?`; sem acentos). Bombeia eventos a cada chamada (o SO nao marca "nao responde").
  Ex.: `win_progress(30, "Compilando 4/16...");`
- **Icone e titulo:** icone 32x32 ARGB gerado em codigo (Kirby rosa; `win_icon_pixels` para teste) aplicado
  com `SDL_SetWindowIcon` na janela do jogo e na de progresso. `WIN_TITLE` =
  "Kirby & The Amazing Mirror - Recomp" e e o padrao quando `win_open(NULL, escala)`. **Nasus:** passe
  `NULL` (hoje `rt_window.c` passa "Kirby & The Amazing Mirror").
- **Tela cheia:** `win_set_fullscreen(on)` (desktop, sem trocar resolucao), `win_is_fullscreen()`, F11 alterna
  dentro de `win_poll`. Escala logica 240x160 'nearest' mantida (barras pretas ao esticar).
- **kirby.ini** (`hw/config_ini.h`, sem SDL): `KirbyConfig {scale 1..6 (3), mute, fullscreen, vsync}`;
  `cfg_defaults`, `cfg_load` (0 ok, 1 inexistente=padroes, <0 erro), `cfg_save` (CRLF, normaliza),
  `cfg_path_beside(arquivo, out, cap)` = `kirby.ini` no diretorio do save/ROM. Formato `chave=valor`,
  comentarios `;`/`#`, `[kirby]` opcional, chaves desconhecidas ignoradas, valor invalido => padrao
  (`scale=2x`, `scale=9`). Aplicacao: `win_open(NULL, c.scale)`, `if (c.fullscreen) win_set_fullscreen(1)`,
  `c.mute` -> nao chamar `audio_open`, `c.vsync` -> `win_open_ex(..., 1)`.
- **Teste** `hw/tests/test_launcher_ui.c` (`run_demo.ps1 -Demo ui`, headless com driver dummy, OK): ini (padroes,
  ida/volta, comentarios, invalidos, normalizacao, caminho ao lado do save), icone (transparencia, cores,
  624 px opacos), titulo, drop (uma vez / substitui), tela cheia, progresso (0% = 0 px, 50% = 4360 px, 100% =
  8720 px de 8720; >100 e <0 limitados; texto 844 px; SDL_QUIT => 0). Saidas para olhar:
  `build/hw/ui_progress.ppm|png`, `build/hw/ui_icon.ppm|png`.
- **Nao feito/limites:** drop testado com evento sintetico (`SDL_PushEvent`), nao arrastando de verdade no
  Explorer; tela cheia verificada so no driver dummy; sem selecao de ROM por dialogo (so soltar/CLI);
  gravar o ini quando o usuario muda algo em tempo de execucao e do runtime (chamar `cfg_save`).

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
Estado: **crt0 (0x080000C0) -> main -> 3000 quadros (titulo e cutscene na tela) recompilado,
sem abortar**, com `hw/` ligado e IRQ de VBlank real (secao 'hw/ ligado + quadros' abaixo).
~5660 funcoes do mapa (~593 mil instrucoes) viram C; 2 nao decodificam.

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

### hw/ ligado + quadros (Nasus) - `python -m recomp.program --frames N`
**Passou:** 3000 quadros sem abortar; 3001 IRQ de VBlank entregues (o handler do jogo, em IWRAM
0x030017F0, ja faz IF e BIOS_IF: "pelo runtime=0"); 0 enderecos de ROM nao resolvidos; SWI:
05 x3000, 06 (Div) x8500, 08 x8, 0B x352, 0C x1, 12 x16. O framebuffer (`hw_frame_ready`)
mostra a tela de titulo (~q.600) e a cutscene de abertura com texto (~q.2000); ~q.3000 e
preto (era uma transicao: ver 'Fade, teclas...'). `--ppm` grava o ultimo quadro.
- `hw/hw.c` e linkado no `rt_boot` (forte) e substitui os stubs fracos de `gba_bus.c`.
- `VBlankIntrWait`: `hw_step(1232)` + `hw_frame_ready` + entrega ate `BIOS_IF&1`; guarda de
  3 quadros (aborta com dump se nenhum VBlank chegar).
- IRQ: `rt_irq_service` chama o vetor [0x03007FFC] como a BIOS (modo IRQ, r0=0x04000000,
  lr=0x138, salva/restaura r0-r12/flags/modo/thumb). Se o handler nao limpar IF/BIOS_IF o
  runtime limpa/OR (contadores impressos). IRQ tambem entra em "pontos seguros": a cada
  chamada de funcao (`rt_tick`, ~24 ciclos/chamada) e a cada 1024 desvios para tras
  (`rt_tick_loop`, 8192 ciclos) - esperas por flag/VCOUNT em laco nao travam.
- `bl` agora registra o retorno esperado (`rt_rets`); se a pilha guest "salta" frames (`pop
  {r0}; bx r0` de epilogo compartilhado, agbcc) `rt_run_until` desenrola os frames C ate o
  que espera esse endereco. Antes disso o 1o IRQ abortava em 0x138.
- `bl` para alvo que e continuacao (salto longo do agbcc): `continuations.csv` -> `goto` no
  corpo do pai (+ `lr` setado). 49 pares.
- Nao resolvidos em ROM: gravados 1x em `build/recomp/program/unresolved.log` e tratados
  como `bx lr` (APROXIMACAO; o log diz onde). `program.iterate` usa esse log como semente:
  descobre a funcao, recompila so os shards afetados e repete (`--stop-on-unresolved` nas
  rodadas). Achou 14 handlers do M4A que o mapa nao tem (`mov ip,lr ... bx ip`):
  0814F938 0814F944 0814F958 0814F96C 0814F99C 0814F9B0 0814F9C4 0814F9D8 0814F9EC 0814FFC0
  0814FFCC 0814FFE0 0814FF64 0814F8D8 (persistidos em `extra_functions.csv`; **Poppy/Riven:
  incluam no mapa**). Sem eles o jogo roda um laco infinito no player de trilhas: tratar
  "retorna" como aproximacao e perigoso.
- Overlays: agora por (endereco, hash FNV do corpo); SRAM_V113 copia varias rotinas para o
  mesmo endereco da pilha (0x03007DA0/0x03007D58) -> variantes `fn_03007DA0_v1`. Funcao ARM e
  Thumb dentro da mesma regiao (0x03000060: SoundMainRAM) = 2 entradas, escolhidas por modo.
- Decoder: `umull/umlal/smull/smlal` (a rotina 0x0814F3DC usa) - fuzzado contra o Unicorn.
- Watchdog (`--timeout S`): imprime ultimo desvio para tras, regs e ultimas chamadas.

### Fade, teclas, quadros, save (Nasus)
**Quadro preto ~q.3000 = transicao normal, nao quebra.** Sem teclas: q.2900 DISPCNT=1340,
BLDALPHA=0010, 312 pixels nao pretos (cutscene sumindo); q.3000 DISPCNT=1240, BLDALPHA=1000,
0 pixels, paleta 0 = 03E0; q.3100 DISPCNT=1440, pal0=0000, 38.3 mil pixels (tela nova, ~q.3100-4000
= cena seguinte, q.5000 = DISPCNT 1740 + BLDCNT 0641, cena com sprites). BLDCNT=0000 e BLDY=0000
em toda a janela 2900-3000 => o fade e feito pela paleta/DISPCNT (jogo zera o BG), nao pelo
hardware de blend. Nenhuma mudanca foi feita para "passar".
- `--keys "START@620+8,A@800"` (tecla@quadro[+duracao]); `hw_set_keys` por quadro (ativo em
  0). Com `START@620,A@800,A@1000,A@1200,START@1400,A@1600,A@1900`: titulo -> menu -> **1a fase
  na q.2000** (DISPCNT=1B40, BG0-3+OBJ; 4 Kirbys e HUD; estavel ate q.8000).
- `--snap 600,2000,...` grava `build/recomp/frames/frame_NNNNN.ppm` (via `hw_get_framebuffer`,
  nao `hw_frame_ready`) e imprime DISPCNT/BG*CNT/BLD*/MOSAIC/WIN*/pal0/IE/IME e pixels nao pretos.
  `python -m recomp.program --frames 8000 --keys ... --snap ...`.
- Save: `<rom>.sav` (ou `--sav F`, `--sav none`) = 32 KiB crus, 0xFF se novo, gravado no
  `atexit`. O jogo escreveu 4736 bytes no primeiro boot (formatacao do save). Barramento 0x0E.. e
  32 KiB espelhado (era 64 KiB).
- `ReadSramFast` (0x0815917C) e alias de `ReadSram` (0x08159158): no emissor (`ALIASES`) e em
  `rt_call` (chamadas por ponteiro). Nao recompila a copia para a pilha; Write/Verify ainda
  usam overlays em 0x03007Dxx.
- Handlers do M4A: `build/indirect/m4a_handlers.csv` (17, incluindo F9F8/FA10/FA24) e lido como
  semente ate a Poppy reingerir; `unresolved.log` continua alimentando `extra_functions.csv`.
- **Tabelas de salto:** `jumptables.csv` agora entra no `discover` (alvos do `switch` viram parte
  da funcao). Sem isso, 0x08070164 tinha 161 de 276 instrucoes e abortava ("saiu sem avancar
  o pc") ao atingir o `case` em 0x08070280. Medido: 593.4k -> 607.9k instrucoes recompiladas.
- Resultado: **8000 quadros com teclas, 0 abortos, 0 nao resolvidos**, 8001 IRQ; SWI 05 x8000,
  0B x555, 11 x1, 12 x15, 14 x2. Um unico aviso do barramento: `read8 0x100F0D55` (regiao
  inexistente; ponteiro lixo vindo do jogo ou de um erro nosso - NAO investigado).

### Janela, ritmo, velocidade, audio (Nasus)
- **Abrir e jogar:** `python -m recomp.program --window [escala]` (recompila se preciso, converge o
  mapa em 700 quadros sem janela e abre `build/recomp/program/kirby.exe`). Direto:
  `kirby.exe baserom.gba --window 3 [--sav F] [--keys ...] [--snap ...]`. `kirby.exe` + `SDL2.dll`
  vem do mesmo build (`build_window`; SDL2 devel MinGW em `C:\libs\SDL2-2.32.2_64-w64-mingw32`
  ou `SDL2_DIR`; linka a import lib `libSDL2.dll.a`, nao a estatica). `rt_boot.exe` segue sem
  janela (testes, `--keys/--snap/--frames`).
- **Teclas** (definidas em `hw/window_sdl.c`): Z=A, X=B, Backspace=SELECT, Enter=START, setas,
  S=R, A=L, Esc ou fechar a janela = sair (grava o save no `atexit`). Com `--keys`, o roteiro
  tambem vale (mesclado depois da leitura da janela).
- **Ritmo:** 59,7275 quadros/s (280896 ciclos @ 16,78 MHz) com `QueryPerformanceCounter`; dorme
  `Sleep(1)` so enquanto faltar >2 ms e gira no resto; se ficar >5 quadros atrasado, ressincroniza.
  Nao dorme se o PC for mais lento (so conta "quadros atrasados"). Teste (300 quadros, janela
  escala 2): 5,40 s para 5,02 s ideais - os ~0,4 s extras sao abertura da janela; 7 quadros
  atrasados (os primeiros, com compilacao de SDL/janela).
- **Velocidade real, sem limite de tempo** (`--bench --bench-from N`, 2600 quadros com o roteiro
  de teclas ate a 1a fase): total **580 quadros/s (9,7x tempo real)**; trecho de fase q.2000..2600:
  **563 quadros/s (9,4x)**. O custo e **87% `hw_step`** (PPU por linha + FIFO/timers + DMA):
  438.823 chamadas, 1672 ciclos/chamada media, 3,90 s dos 4,48 s; o codigo do jogo recompilado leva
  so 0,58 s. Ou seja, o gargalo nao e a recompilacao; se precisar de mais folga, otimizar o
  render do `hw/` (passos de <=128 ciclos = ~2200 iteracoes/quadro, Gwen). Folga atual >9x.
- **Audio:** por quadro chamo `hw_audio_read(buf, 4096)` (rate 48 kHz, ~804 amostras/quadro) e
  entrego a `audio_push(buf, n)` se existir (simbolo fraco); senao descarta. Medido: 2,10 M
  amostras estereo em 2600 quadros = 43,7 s de audio para 43,5 s de jogo (consistente).
  Saida real de audio: pendente da Gwen (`audio_push`).
- **Leitura em 0x100F0D55** (impressa uma vez com contexto; `rt_window.c: gba_bus_unmapped`):
  `read8` com r0=0x100F0D55; lr=0x080339D1, sp=0x03007D8C, thumb; r1=08D6CD0C r2=08D60EE8
  r3=02020EE0 r4=000001A8 r5=0203AD3C r6=08D60A80 r7=0600E4E8 r8=02020EE0 r9=08D60EE4
  r10=08D6CD0C r12=02000000. Pilha de retornos esperados (interna -> externa): 080339D0
  080333CE 08124026 08142686 08141EB2 08152D3A 08151CF8 08152A12; ultima chamada direta
  08034FA8 (`pc` exato nao e mantido pelo codigo gerado; a pilha e o melhor indicio:
  funcao que retorna para 0x080339D0 = quem le o byte). 0x100F0D55 nao e endereco de GBA:
  provavelmente soma de ponteiro + indice com base errada (r4=0x1A8 e indice) - investigar a
  funcao em ~0x08034FA8 / chamador 0x080339D0.
- Rodadas do `iterate` agora usam o ULTIMO dump (`pc=`) no stderr (dumps informativos de
  diagnostico apareciam antes e confundiam a descoberta de overlays). Novo overlay achado
  em fase: 0x03007D18 (WriteSram na pilha com outro sp) e 0x030001F8 (ARM, SoundMainRAM).

### Som no kirby.exe (Nasus)
- `kirby.exe` agora linka `hw/audio_sdl.c` (mesma `libSDL2.dll.a`). `rt_window_install` chama
  `audio_open()` (se <0: avisa 1x "audio: sem saida" e segue mudo); por quadro
  `hw_audio_read(buf,4096)` -> `audio_push(buf,n)`; no fim imprime
  `AUDIO: underruns=.. descartados=.. (fila ..)` e chama `audio_close()`. `--mute` (tambem em
  `python -m recomp.program --window --mute`) desliga. O `rt_boot.exe` (sem SDL) mantem o
  `audio_push` fraco e descarta.
- **1 minuto de jogo** (3600 quadros, roteiro de teclas ate a fase, ritmo 59,7275, escala 2):
  60,80 s para 3600 quadros (59,2 q/s; 3 quadros atrasados); 2.902.545 amostras lidas = 60,5 s
  de audio; **underruns=2, descartados=877 quadros de audio (18 ms no total, 0,03%)**, fila
  final 7844 quadros (~163 ms, perto do teto de ~200 ms: o produtor anda um pouco a frente do
  relogio da placa e a fila segura a latencia descartando o mais antigo). Nao ha underruns
  constantes; os 2 sao da partida. Quem ouve: nao conferi a qualidade a ouvido (sem acesso ao
  alto-falante daqui) - so contadores. PSG ainda mudo (Gwen).
- Lembrete: o `iterate` precisa ter visto o trecho antes de abrir a janela (overlay 0x03007D18 da
  SRAM so aparece apos ~q.2000 com o roteiro): `python -m recomp.program --frames 3600 --bench
  --keys "..."` uma vez, depois `--window`.

### Estalo no som (esquerdo, ~1/s) - causa e correcao (Nasus)
- Relato (ouvido): "." no canal esquerdo cerca de 1x/s. Os contadores do teste anterior
  (`descartados=877`, fila final 163 ms ~ teto de 200 ms) ja apontavam: a fila ficava no teto e
  `audio_push` descartava amostras do inicio da fila a cada quadro -> degrau no sinal. Como o
  canal direito estava quase constante naquele trecho, o degrau so e audivel no esquerdo.
- Conferido nos dados (`--audio-raw F` grava o PCM cru; 3000 quadros): os saltos grandes
  isolados do L (18688 = 73*256, em t=0.30, 0.50, 0.90, 1.10 s...) sao ataques de notas/
  percussao da musica (decaem em padrao de 4 amostras a ~18 kHz, repetido 3x a 48 kHz), nao
  corrupcao: o `hw_audio_read` e o mixer recompilado estao coerentes.
- Correcao (`rt_window.c`): **o relogio da placa e o mestre**. Em vez de ritmo por QPC, com som
  ligado o laco espera a fila drenar ate ~4 quadros de video (3200 amostras, 67 ms) antes do
  proximo quadro (dorme 1 ms por vez; limite 100 ms). Sem som/`--mute`, volta ao QPC.
- Medido (1 min = 3600 quadros): **descartados=0**, underruns=2 (ambos no quadro 1, antes de
  haver dado), fila entre ~2600 e 3100, 59,1 q/s, 0 quadros atrasados. Antes: 877 descartes.
  A confirmar de ouvido por quem tem o alto-falante.

### SRAM na pilha em qualquer sp (Nasus) - correcao do crash da 1a porta
- Em `rt_call`, endereco em IWRAM/EWRAM (0x02/0x03), Thumb, sem funcao/overlay: compara os bytes
  em `pc` com os corpos ROM das rotinas SRAM copiadas para a pilha - **ReadSram 0x08159158 (34
  bytes, ate o `bx r0`)** e **VerifySram 0x08159220 (48 bytes, ate o `bx r1`)**; compara
  exatamente o tamanho do corpo (nao le alem). Casou: `pc = endereco ROM` e chama a funcao
  recompilada com os mesmos registradores; o retorno (`pop {r1}; bx r1`) volta ao `lr` normal.
  Nao casou: abort com pc/lr/regs como antes. WriteSram (0x081591E0) nao e copiada e usa
  `ldr [pc]` (fica de fora, nao e posicao-independente).
- Vale para qualquer `sp` (as variantes 0x03007DA0/0x03007D58/0x03007D18/0x03007CEC e o que vier):
  nao depende mais do `iterate` ter visto aquele sp.
- **Teste:** `rt_boot.exe baserom.gba --selftest-sram` copia ReadSram e VerifySram para 4
  enderecos (0x03007CEC, 0x03007DA0, 0x03007B10 e um em EWRAM 0x02001230), executa cada uma via
  `rt_run_until` e confere r0 (0 se igual, endereco do 1o byte diferente) e que `sp` volta
  igual; Read confere os 8 bytes copiados: **OK, 0 falhas** (12 usos do alias).
- Jogo real: roteiro de teclas ate o hub e 6000 quadros: **16 usos do alias**, 0 abortos, 0 nao
  resolvidos, save com 4736 bytes escritos (formatacao inicial; muda como antes). **Nao cheguei
  a abrir a 1a porta** (meu roteiro so anda para a direita no hub; nao sei o ponto exato da
  porta) - quem reproduziu o crash deve rodar o mesmo trajeto com este build e confirmar que
  a fase seguinte carrega.

### Interpretador de fallback (Nasus) - `recomp/runtime/rt_interp.c`
- **O que e:** interpretador ARM/Thumb (ARMv4T) em C, mesma semantica do emissor: PC le +8/+4
  (+12 com shift por registrador e em STR/STM), LDR rotaciona, STR/STRH alinham, LDM/STM,
  B/BL/BX, MUL/MULL, MRS/MSR, SWI -> HLE, laco com `rt_tick_loop` (tempo e IRQ). Sem LDRD/SWP/
  coprocessador (ignorados). O decoder e reescrito em C (decode.py nao roda no runtime); a
  fidelidade vem do teste diferencial abaixo.
- **Quando entra:** `rt_call` sem funcao recompilada -> (1) alias SRAM; (2) registra 1x em
  `unresolved.log` (`ENDERECO,modo,lr=,sp=,body=<32 bytes em hex>`); (3) interpreta ate o `pc`
  cair numa funcao conhecida ou em um retorno esperado por frame C externo (`rt_rets`/IRQ).
  `bl` para funcao compilada vira chamada C normal e a interpretacao continua depois dela.
  Vale para RAM (overlay desconhecido) e ROM fora do mapa (antes: "retorna", aproximacao).
  Fora das duas regioes (ex.: 0x100F0D55, saltos lixo) ainda aborta.
- **Abort que sobra** imprime os **64 bytes em pc + hash FNV-1a (64 e 32 bytes)**, alem de
  pc/lr/regs/ultimas chamadas, para decidir overlay ou alias:
  `[rt] SEM FUNCAO em XXXXXXXX (thumb): 64 bytes=...` / `hash do corpo ...`.
- **Construcao (`iterate`) nao muda:** usa `--stop-on-unresolved` (aborta no 1o miss, ROM ou RAM)
  para continuar descobrindo overlays/handlers; o fallback so atua em execucao normal/`kirby.exe`.
- **Testes** (`rt_boot.exe baserom.gba --selftest-interp`):
  1. corpo de VerifySram copiado para 3 enderecos da pilha (0x03007CEC, 0x03007B80, EWRAM
     0x02002340), alias desligado: o fallback da r0/sp/r4 **iguais** ao nativo (igual e diferente).
  2. **diferencial**: 150 funcoes recompiladas da ROM (<=0x100 bytes) x 6 estados aleatorios,
     compilado x interpretado (r0-r14, NZCV, pc, modo, EWRAM+IWRAM): **571 execucoes comparadas,
     0 divergencias** (23 com limite de laco/abort ignoradas), 123 M instrucoes interpretadas.
  3. **jogo inteiro forcado ao interpretador** (`--no-sram-alias --no-overlays`: mixer SoundMainRAM,
     handler de IRQ em IWRAM e rotinas SRAM todos interpretados): 6000 quadros, 12072 entradas,
     141 M instrucoes, 0 abortos; **quadro final (PPM) e audio PCM (`--audio-raw`) byte a byte
     iguais** aos da execucao compilada (4000 quadros, md5 identicos).
- **Custo:** nada novo acontecendo = 0 entradas, 0 instrucoes (546 q/s, 9,1x). Com TUDO em RAM
  interpretado: 442 q/s (7,4x), ainda acima de 60. Sem impacto em `--keys`/janela.
- Limite: 50 M passos por entrada (so avisa 1x e continua no proximo `rt_call`).

### Latencia de IRQ de VBlank: hw_step_irq + relogio congelado no handler (Nasus)
- Pedido da Gwen (estalo do canal esquerdo = DMA do M4A lendo 16 bytes alem do buffer PCM
  quando o reinicio cai depois de um pedido de FIFO). So `gba_bios.c`:
  1. `rt_tick`, `rt_tick_loop` e a espera de `VBlankIntrWait` avancam o hw por `run_cycles()`:
     `hw_step_irq(left)` ate gastar tudo, **parando no instante em que um IRQ habilitado fica
     pendente**; a cada parada `grab_frame()` + `rt_irq_service()` entregam o IRQ na hora (latencia
     ~0). Se `hw_step_irq` devolve 0 (IRQ pendente mas nao entregavel, I=1) avanca com `hw_step`
     para nao travar. `rt_hwstep_calls/cycles` continuam contados (`timed_step_irq`).
  2. **Relogio congelado durante o handler** (`in_irq`): `rt_tick` (24 ciclos/chamada) e
     `rt_tick_loop` retornam sem avancar enquanto o handler de IRQ executa, entao
     `m4aSoundVSync` roda sempre em VBlank+0.
- Medido depois (6000 quadros, roteiro de teclas): 590 q/s (9,9x), 1,02 M chamadas de
  `hw_step_irq`, 1658 ciclos/chamada; 6001 IRQ. Compilado x interpretado (RAM forcada ao
  interpretador): quadro e audio PCM continuam **byte a byte iguais**; `--selftest-interp` 0
  divergencias. Nao medi o numero de pedidos de FIFO por periodo (99) nem ouvi: isso e do lado
  da Gwen (precisa do build novo `kirby.exe`).

### Tabelas de salto completas, AVISO e alvo fora do conjunto (Nasus)
- **Mapa regerado (Poppy):** `jumptables.csv` com `bounded`; 4 tabelas antes truncadas em 64.
  Conferido com `discover(..., indirect=jt)` (loader ja le o CSV): **todos os alvos de cada tabela
  estao no conjunto de instrucoes da funcao dona**:
  | tabela | entradas (distintos) | funcao dona | insns | alvos fora do conjunto |
  |---|---|---|---|---|
  | 0x0809DA50 | 214 (6) | 0x0809DA30 | 648 | 0 |
  | 0x0809A348 | 85 (3) | 0x0809A1C4 | 265 | 0 |
  | 0x0800B4F0 | 100 (13) | 0x0800B414 | 215 | 0 |
  | 0x08076254 | 67 (9) | 0x08075EDC | 1147 | 0 |
  `0x0809DE38` (onde a sessao de espada + celular parava) **esta no conjunto** da 0x0809DA30.
  Recompilado (5669 funcoes, 608.168 insns): 3600 quadros, 0 abortos, 0 nao resolvidos,
  `--selftest-interp` 0 divergencias. **Nao reproduzi a sessao "espada + amigos" por teclas**
  (nao sei o trajeto): a verificacao acima e estatica + regressao; confirmar jogando.
- **AVISO:** o contexto da leitura em 0x100F0D55 agora sai como `[rt] AVISO: contexto do acesso
  sem regiao ...` (`rt_dump_label`); "PARADA" so quando o processo realmente para.
- **"funcao saiu sem avancar o pc (alvo fora do conjunto)" nao aborta mais:** loga 1x em
  `unresolved.log` (`ADDR,modo,lr=,sp=,body=,kind=outside-set,fn=LO..HI`) e entrega o `pc` ao
  interpretador, que nao sai da faixa da funcao ate o fluxo voltar a uma funcao/retorno
  conhecido. `--stop-on-unresolved` (construcao) continua abortando. Teste
  (`--selftest-interp`): 400 entradas no meio de 60 funcoes, 17 cairam fora do conjunto e foram
  interpretadas sem derrubar o processo (os 334 abortos contados sao saltos aleatorios para
  regiao invalida, esperado com registradores lixo).

### Distribuicao: caminhos, kirby.exe sem argumentos, interpretador puro, tempo de compilacao (Nasus)
**1. Caminhos (nenhum depende de `.venv`/`C:\libs`/cwd do repo)** - `recomp/toolchain.py`:
| item | ordem de busca |
|---|---|
| zig | `$KIRBY_ZIG` -> `<raiz>/tools/zig/zig(.exe)` -> `<pasta do launcher>/tools/zig` -> `zig` no PATH -> `python -m ziglang` (so dev) |
| SDL2 | `$SDL2_DIR` -> `<raiz>/sdl2` -> `<raiz>/tools/sdl2` -> `C:/libs/SDL2-2.32.2` (aceita a subpasta `x86_64-w64-mingw32`) |
| saida/cache (`OUT`) | `--out DIR` -> `$KIRBY_OUT` -> `<raiz>/build/recomp/program` (se ha `.git`) -> `%LOCALAPPDATA%/KirbyRecomp/<sha1 da ROM>` |
| ROM | `--rom` -> `baserom.gba` no cwd -> launcher -> raiz |
| mapa | `--functions` -> `<out>/analysis/functions.csv` -> `<raiz>/build/analysis/` (csv irmaos pela pasta do mapa) |
`python -m recomp.program --print-paths` mostra o que foi achado. `harness.zig_cc()` usa o mesmo
localizador (sem `sys.executable -m ziglang` quando ha zig). Frames/log/save ficam em `<out>`.
**2. `kirby.exe` sem argumentos** (`rt_main.c`): ROM = `argv[1]` (arrastar sobre o exe) ou, sem
argumentos, `baserom.gba` ao lado do exe -> primeiro `*.gba` ao lado -> `baserom.gba` no cwd ->
ultima ROM (`kirby_lastrom.txt` ao lado do exe, gravado ao abrir). So a ROM/nada = modo jogo:
janela escala 3, som, save em `<rom>.sav`. Nao achou: MessageBox com instrucao. **F11** alterna
tela cheia (`rt_window.c`, `SDL_WINDOW_FULLSCREEN_DESKTOP`, borda de subida). Testado: arrastar
(caminho com espacos), duplo clique com ROM ao lado, a partir de outro cwd, e sem ROM (mensagem).
Escala > 3 e mudo continuam por `--window N`/`--mute`. *Soltar a ROM na janela (SDL_DROPFILE)
precisa de `hw/window_sdl.c`: nao e meu (pedido a Gwen).*
**3. Interpretador puro** (`--interp-all`: nenhuma funcao "conhecida"; `--interp-only` gera um
exe SEM o jogo compilado, so runtime + `rt_interp`): 3000 quadros com o roteiro de teclas (menu ate
a 1a fase), mesma maquina (6 nucleos):
| modo | q/s | x tempo real | instrucoes interpretadas |
|---|---|---|---|
| compilado | 565 | 9,5x | 0 |
| `--interp-all` | 361 | 6,0x | 257 M |
| `--interp-only` (exe separado) | 311 | 5,2x | 252 M |
O custo e metade `hw_step` (52-56%) e metade interpretacao (~4,6 s de 9,7 s). **>= 60 q/s com folga
de 5x -> viavel "iniciar ja interpretando enquanto compila".** `--interp-only` compila em
**1,8 s** (runtime+hw+SDL; com cache do zig). Video igual byte a byte ao compilado; audio igual em
99,4% dos quadros estereo (15 185 de 2,42 M diferem, desalinhamento de 1-2 amostras: o
interpretado soma ticks de hw por `bl` mas nao e identico ao compilado em casos de veneer/stub;
inaudivel nos testes, nao investigado a fundo).
*Proposta de fluxo (nao implementada):* o launcher (1) abre o jogo com `kirby.exe --interp-only` em
~2 s; (2) compila o jogo em segundo plano (3 min com `-Os`; o interpretador usa ~1/5 de um nucleo a 60
q/s, sobra CPU); (3) **troca sem perder o estado carregando o compilado como DLL no MESMO processo**:
estado = `Cpu` + EWRAM/IWRAM/SRAM (runtime) + `hw/` (tudo no processo), logo basta trocar
`rc_table` num ponto seguro (fim de `VBlankIntrWait`, sem frame C ativo) por `LoadLibrary` da DLL
gerada. Requer: shards compilados como DLL; o runtime exportado por uma tabela de ponteiros
(`RtApi`: `rc_ldr32`, `rt_call`, `rt_tick`, `rt_rets`...) passada a `rc_init(const RtApi*)` em vez de
simbolos externos (macros no `gba_cpu.h` do modo DLL); lookup passa a consultar a tabela da DLL
primeiro. Alternativa sem DLL: reiniciar o exe no proximo "soft reset"/menu (perde estado) ou
`hw_save/hw_load` (Gwen) + save-state do runtime.
**4. Tempo de compilacao a frio** (6 nucleos, 608 mil instrucoes, 164 MB de C, 1 construcao
do zero com `--out` novo; "total" inclui emissao, rodadas do `iterate` e link):
| config | compilado | total | objetos |
|---|---|---|---|
| `-O1`, 16 shards, 8 jobs (antes) | 228-292 s | ~5 min | ~400 MB (-O0: 376 MB so em 4 shards!) |
| `-O1`, 48 shards, 6 jobs | 191 s | 3m49 | ~200 MB |
| **`-Os`, 48 shards, 6 jobs (novo padrao)** | **146 s** | **3m01** | **~45 MB** |
Micro-benchmark de 4 shards: `-O0` 87,5 s (objetos enormes: 376 MB), `-O1` 65,7 s, `-Os` 45,6 s. `-O0`
nao ajuda. Velocidade do jogo com `-Os`: 569 q/s (700 q) contra 642 q/s com `-O1` (hw_step domina; 13%
a menos, sobra de 9x). Runtime nao recompila (objetos em cache por hash); so 7 .c pequenos. Opcoes:
`--opt=-Os|-O1`, `--shards N`, `--jobs N`. O "~10 min" do plano do Riven nao reproduziu aqui; em PC
com 2-4 nucleos espere ~2-3x do tempo. Nao medido: zig frio (1o uso compila compiler-rt/libc no
cache global do zig, +alguns s).

### O que falta / riscos
- Interpretador de fallback: endereco de RAM sem overlay ainda **aborta**; ROM sem funcao
  vira "retorna" (logado). Codigo em RAM que mude depois nao e achado sozinho fora do
  `iterate`.
- Sem teste diferencial de `bl`/pilha/IRQ contra um oraculo de programa inteiro (so o fuzz
  por instrucao + 40 funcoes + boot). Um oraculo completo (Unicorn + hw/ via ctypes) seria o
  proximo passo para pegar erros silenciosos como o do player de trilhas.
- SRAM/save, som real (FIFO/timers dependem do `hw_step`) e entrada nao foram exercitados.
- Compilacao de 600k instrucoes em C: 4-5 min a frio; depois cache por shard.
- ArcTan2 e RegisterRamReset: ainda sem conferencia com a BIOS real.

## Som (M4A) e save (SRAM) (Riven)
Fonte: desmontagem a mao dos enderecos abaixo (capstone) + ram_code.csv da Poppy. E o motor
**M4A / "MusicPlayer2000"** padrao da biblioteca de som da Nintendo, nao BIOS (o jogo nao usa
SoundDriver* da BIOS). Estrutura `SoundInfo` apontada por `[0x03007FF0]`; campo `ident` =
`0x68736D53` ("Smsh"), que e trocado/incrementado para travar o mixer durante a mixagem.

### Enderecos (ROM)
| Funcao | Endereco | O que faz |
|---|---|---|
| `m4aSoundInit` | 0x08150098 | copia o mixer p/ RAM (CpuSet), chama SoundInit, inicia players |
| `SoundInit` | 0x08150498 | para DMA1/2, SOUNDCNT, FIFO A/B, grava `[0x03007FF0]`, ident |
| `m4aSoundMode` | 0x08150590 | calcula taxa, configura Timer0, sincroniza em VCOUNT=159 |
| `m4aSoundMain` (wrapper) | 0x08150110 -> `SoundMain` 0x0814F3E8 | mixa um periodo no buffer PCM; salta p/ 0x03000061 (mixer em RAM, Thumb) |
| `m4aSoundVSync` | 0x0814FA3C | por VBlank: reinicia DMA1/2 a cada `pcmDmaPeriod` quadros |
| `VSyncOn` / `VSyncOff` | 0x0815079C / 0x08150720 | liga/desliga DMA de som e limpa o buffer |
| Mixer em RAM | ROM 0x0814F46C -> IWRAM 0x03000060 (1024 B) | vem do `ram_code.csv` (bloco 0) |
`m4aSoundVSync` e chamado de 0x08152388 (provavel handler de VBlank). Dispatcher de IRQ do jogo:
0x08000100 (ARM) le IE&IF e salta pela tabela em IWRAM `0x030017B0` (4 bytes por fonte; VBlank
= indice 1); bit 13 (gamepak) trava em loop (`b .`).

### O que o motor pede ao hardware (valores lidos no codigo)
| Registrador | Valor | Significado |
|---|---|---|
| SOUNDCNT_X 0x04000084 | 0x008F | master enable (+ bits de status 1-4) |
| SOUNDCNT_H 0x04000082 | 0xA90E | PSG vol 100%; DMA A e B vol 100%; **A -> direita, B -> esquerda, ambos Timer0**; reseta FIFO A e B |
| SOUNDBIAS 0x04000089 (byte alto) | `(b & 0x3F) \| 0x40` | resolucao 8 bit / ciclo de amostra 65,5 kHz |
| FIFO_A / FIFO_B 0x040000A0 / A4 | via DMA1 / DMA2 | 32 bit, destino fixo |
| DMA1SAD/DAD, DMA2SAD/DAD | SoundInfo+0x350 -> 0x040000A0; SoundInfo+0x980 -> 0x040000A4 | os dois buffers PCM (0x630 = 1584 B cada) |
| DMA1CNT_H / DMA2CNT_H (0x040000C6 / D2) | 0xB600 | enable, repeat, 32 bit, dest fixo, inicio = FIFO especial |
| Parar DMA | grava 0x84400004 em DMAxCNT (32 bit) e depois 0x0400 em CNT_H | usado em SoundInit / VSyncOff / VSync |
| TM0CNT_L 0x04000100 | `-(0x44940 / 264)` = -1064 | 1064 ciclos por amostra (prescaler 1) |
| TM0CNT_H 0x04000102 | 0x0080 | habilita Timer0, **sem IRQ** |
| VCOUNT 0x04000006 | espera sair e voltar a 0x9F (159) | para iniciar o Timer0 alinhado ao quadro |
Modo de som (literal 0x0095FA00 em m4aSoundInit): 10 canais, volume 15, DAC 8 bit, indice de
frequencia 5 (**15768 Hz**, 264 amostras por quadro); pcmDmaPeriod = 1584/264 = 6; pcmFreq =
264 * 59,7275 Hz. (CORRIGIDO: a 1a versao dizia 18157 Hz/304/5 por eu ter lido o indice 5 como a 6a entrada
da tabela; a tabela em ROM e 1-based: [96,132,176,224,**264**,304,...].) Tabela de amostras/quadro em ROM 0x08B58604 (u16).
Canais PSG (SOUND1..4, WAVE_RAM) tambem sao tocados pela engine (funcoes 0x08150354,
0x08150634, 0x08150B98, 0x08150C50, 0x08157210, 0x0803A1F4): implementar depois da FIFO.

### O que a Gwen precisa fornecer (ordem sugerida)
1. FIFO A/B de 8 palavras (32 bytes), amostras assinadas de 8 bit. Timer0 estourando consome
   1 amostra de cada FIFO ligado ao Timer0. Quando sobram <= 16 bytes, DMA1/DMA2 em modo
   especial (CNT_H = 0xB600) transfere 4 palavras de SAD (incrementando), destino fixo.
2. Saida: esquerda = B, direita = A (SOUNDCNT_H 0xA90E). Reamostrar 15768 Hz -> 48 kHz.
3. Timer0 em 1064 ciclos/amostra (16,78 MHz / 1064 = 15768 Hz). Sem timer, nao ha som.
4. PSG (canais 1-4) so depois: as notas "normais" do jogo sao PCM via FIFO.
### O que o runtime (Nasus) dispara por quadro
- VBlank IRQ -> tabela 0x030017B0[1] -> `m4aSoundVSync` (0x0814FA3C) -> `SoundMain` (0x0814F3E8)
  uma vez por quadro (pelo loop principal, via 0x08150110), que escreve 264 amostras/quadro.
- `hw_step` precisa avancar Timer0 e a FIFO ANTES de entregar a IRQ, senao o DMA nunca recarrega.
- O mixer e Thumb em IWRAM 0x03000061: ja listado como overlay (0x03000060) pelo runtime.

### SRAM (biblioteca `SRAM_V113`, string em 0x08D608A0)
Funcoes (ROM, Thumb), todas com WAITCNT SRAM = 3 (`(WAITCNT & 0xFFFC) | 3`, 0x04000204):
| Funcao | Endereco | Papel |
|---|---|---|
| ReadSram (copia byte) | 0x08159158 | `ldrb/strb` de src para dest, `size` bytes |
| ReadSramFast | 0x0815917C | copia a funcao acima para a **pilha** e a executa de la |
| WriteSram | 0x081591E0 | escreve bytes em 0x0E000000+ |
| VerifySram | 0x08159220 | compara bytes, devolve endereco do 1o diferente ou 0 |
- Janela de save: 0x0E000000-0x0E007FFF (**32 KiB**, barramento de 8 bit). Formato minimo:
  arquivo cru de 32768 bytes (`baserom.sav`), criado com 0xFF se nao existir; leitura/escrita
  imediatas, sem atraso.
- Cuidado (Nasus): ReadSramFast executa codigo copiado para a pilha (~0x03007Dxx). Isso e o
  overlay `0x03007D58/0x03007DA0` que voce ja achou; como o conteudo e exatamente a funcao ROM
  0x08159158 (mesmos bytes), da para tratar o salto como alias em vez de recompilar de novo.
- Layout interno dos slots/checksums do jogo: nao estudado (so e preciso persistir o array).

### A confirmar (nao verificado por execucao)
- Que 0x08152388 e o handler de VBlank (so vi a chamada, nao o registro em `0x030017B0`).
- Interpretacao de bits de SOUNDCNT_H/SOUNDBIAS acima e 59,7275 Hz foram deduzidas do manual do
  GBA e do codigo; nenhuma foi conferida rodando.

## Handlers do M4A fora do mapa (Riven) - `build/indirect/m4a_handlers.csv`
Pedido do Nasus (14 funcoes achadas em execucao). Causa: o motor M4A despacha os comandos
da musica por uma **tabela de saltos em ROM 0x08B58490-0x08B5851C** (entradas Thumb), e so
as entradas alcancadas por `bl` estavam no mapa. 17 entradas faltavam: 14 que o runtime ja
viu + 3 que o jogo ainda nao chamou (0x0814F9F8, 0x0814FA10, 0x0814FA24). Todas decodificam
e chegam a um retorno. Lista: 0x0814F8D8, F938, F944, F958, F96C, F99C, F9B0, F9C4, F9D8,
F9EC, F9F8, FA10, FA24, FF64, FFC0, FFCC, FFE0. 0x0814FFC0 nao e alvo direto da tabela
(entrada `ldr r2,[r1,#0x40]`; `ldrb r3,[r2]` = leitura do proximo byte da trilha). A tabela
seguinte (0x08B58520+) e dado (tabela de bytes 0xE0.., nao codigo).
Para a Poppy: ingerir `build/indirect/m4a_handlers.csv` com `--extra-functions` (coluna `addr`).

Correcao em `tools/continuations.py` (falsos positivos apontados pela Poppy): ignora pais com
<= 2 instrucoes (0x0814F890) e pais que contem `svc` (SoftReset 0x0815931C nunca retorna).
Agora o script lista 0 pares: o mapa congelado ja absorveu as 49 continuacoes reais
(`functions.csv` tem a coluna `continuations`), entao o script serve so como verificacao.

## Leitura em 0x100F0D55: origem encontrada (Riven)
**Causa (alta confianca na cadeia, media na conclusao): o proprio jogo le essa posicao quando um
indice vale 0xFFFF; nao e erro do recompilador.**
- Quem le: `0x08034FA8` (chamada em 0x080339CC com r0=0, caminho `r4==0`). Em 0x08035008 faz
  `ptr = [0x08D6CD0C + 4*hw]` e em 0x0803500C `ldrb [ptr + 0x46]`.
- `hw` e o halfword em `obj+0x60`, com `obj = 0x02020EE0 + byte[0x0203AD3C] * 0x1A8` (EWRAM).
- A tabela `0x08D6CD0C` tem **1022 entradas validas** (zeros/ponteiros); depois vira outros dados
  da ROM. Com `hw = 0xFFFF` a entrada lida e `0x08DACD08` = `0x100F0D0F`, e `+0x46` da
  `0x100F0D55` (exatamente o endereco do aviso).
- `0xFFFF` e a sentinela classica "sem objeto/sprite" (u16 = -1); a funcao nao a testa. Em
  hardware real a leitura em 0x10... devolve o valor de barramento aberto, sem travar; o byte
  so vira `r1 = (b << 9) + 0x5300` e o resultado alimenta um CpuSet de 0x80 palavras para VRAM
  (0x060077A0 e 0x060074A0), que depois e sobrescrito.
- **Conclusao:** comportamento do original (codigo que roda com o slot "vazio"). O runtime devolve 0
  para regiao inexistente (contrato do `gba_bus.h`), o que e inofensivo aqui. Nao precisa de
  correcao; se quiser fidelidade, devolver o ultimo valor lido (barramento aberto).
- **Nao verificado:** que o jogo original tambem passa por 0xFFFF nesse ponto (so tenho a cadeia
  estatica + o dump do Nasus). Para confirmar de verdade seria preciso um emulador de referencia.

## Crash ao abrir a 1a porta: rotina SRAM copiada para a pilha (Riven)
Log do usuario: `PARADA: endereco sem funcao recompilada pc=03007CEC lr=081592AD sp=03007CEC
r0=03007DD0 r1=0E000018 r2=8`. (O primeiro "PARADA" do mesmo log, `0x100F0D55`, e so contexto
informativo, ver secao propria; o fatal e o segundo.)
- **O que e:** `0x08159250` e `VerifySramFast`: aloca 0xC0 bytes na pilha, **copia `VerifySram`
  (`0x08159220`, 48 bytes) para `sp`** e salta para la (`mov r2,sp` ... `bx`), porque o SRAM nao
  pode ser lido com codigo rodando da ROM. Portanto `pc == sp == 0x03007CEC`. Argumentos: r0 =
  buffer em IWRAM (0x03007DD0), r1 = 0x0E000018 (SRAM), r2 = 8 bytes. E o jogo gravando/verificando
  o save ao entrar na sala.
- **Por que so agora:** o endereco depende do `sp` no momento da chamada. Ja vimos 0x03007DA0,
  0x03007D58, 0x03007D18; esta e uma variante nova (0x03007CEC). Os overlays sao descobertos so
  no `iterate` offline (700 quadros), entao uma variante que aparece depois da porta aborta.
- **Corpo e posicao-independente:** 24 instrucoes, sem `ldr [pc,..]`; so branches relativos e
  `bx r1`. Por isso qualquer copia, em qualquer `sp`, e identica a `0x08159220` (e analogas
  `0x08159158` ReadSram e as funcoes copiadas por `0x0815917C`/`0x08159250`).
- **Correcao (Nasus):** no despacho, se `pc` cai em IWRAM/EWRAM sem funcao, comparar os
  primeiros bytes (ate o tamanho conhecido) com os corpos das rotinas SRAM da ROM e **chamar a
  funcao nativa da ROM** com os mesmos registradores; retorna por `bx r1`/`lr` normalmente. Vale
  para qualquer `sp`. Se nao casar com nada, manter o abort atual.

## Analise do estalo e do volume no som (Riven) - `tools/audio_spikes.py`
Dados: 70,5 s de jogo gravados pelo usuario com `--audio-raw` (`build/hw/pcm.raw`, 48 kHz, estereo).
**1. Estalo esquerdo = amostras isoladas erradas, so no canal L, travadas ao quadro.**
- 298 eventos em L sem contrapartida em R; 0 equivalentes em R (R limpo). ~1 a cada 14 quadros.
- Largura: uma amostra de 15768 Hz (~3 amostras a 48 kHz), saindo do nivel e voltando.
- **Fase fixa**: 295 de 298 caem no mesmo decimo do quadro (0,7-0,8 do quadro), isto e, no
  **inicio do VBlank** (linha 160 de 228). Nao ha periodicidade de 5 quadros (pcmDmaPeriod).
- Altura tipica: -47 (141 vezes) e +71..+83 unidades PCM (valores de 8 bit; x256 em 16 bit).
- Interpretacao: algo no VBlank (IRQ/`m4aSoundVSync`/`hw_audio_read`) injeta ou repete uma
  amostra errada so na FIFO B (DMA2). Causa nao identificada; candidatos: reinicio do DMA2 no
  VBlank pegando dado do buffer que o mixer esta reescrevendo; ordem DMA1 x DMA2; leitura do
  buffer em hw_step antes do SoundMain terminar.
**2. "Som de dano mais alto": PSG ~2x alto demais em relacao ao PCM.**
- GBATek: "Each of the two FIFOs can span the FULL output range (+-200h). Each of the four
  PSGs can span one QUARTER of the output range (+-80h)". Razao PSG/PCM por canal = 1/4.
- Em `hw.c`: PCM 100% = 256/unidade (+-32768 = escala cheia), PSG vol 15 x master 8 x 100% x 32
  = +-15360 => razao 0,47 (deveria ser 0,25, +-7680). Logo o PSG sai ~1,9x (6 dB) mais alto.
- Nao verificado que o som de dano e PSG; e a explicacao mais provavel para o relato.
- Pico medido: |L|,|R| max ~23000 (70% da escala), sem clipping.

## Crash ao chamar os amigos com a espada; travada ao minimizar (Riven)
Relato do usuario com log (`PARADA: funcao saiu sem avancar o pc (alvo fora do conjunto)`,
`pc=0809DE38 lr=080A390F r1=0809DA50`).
**1. Tabela de salto truncada em 64 entradas (bug do mapa).**
- `0x0809DA30..0x0809DA48`: `ldrb [r0+0x82]; subs r0,#5; cmp r0,#0xD5; bls ...; ldr r0,[tab+4*i]; mov pc,r0`
  => o limite real e `0xD5`, ou seja **214 entradas**. `jumptables.csv` registrou **64**: o
  analisador corta em 64. O alvo `0x0809DE38` e a entrada de um indice > 63, ausente do conjunto
  de rotulos da funcao, entao o runtime parou.
- Mesmo corte em **4 tabelas** (so as 4 com exatamente 64 entradas; ver `build/indirect/jumptable_cap_fix.csv`):
  | site | tabela | tem | limite real |
  |---|---|---|---|
  | 0x0809DA48 | 0x0809DA50 | 64 | **214** (funcao 0x0809DA30) |
  | 0x0809A324 | 0x0809A348 | 64 | 85 (0 alvos novos) |
  | 0x0800B4E0 | 0x0800B4F0 | 64 | 100 (1 novo) |
  | 0x08076248 | 0x08076254 | 64 | 67 (2 novos) |
- Causa no codigo: `add_table(..., last_cmp)` em `tools/analyze_rom.py` usa `limit = 64 if last_cmp is None`.
  Nestes 4 casos o `cmp rN,#imm` fica **antes de um `bls` cujo alvo e o bloco do switch**
  (`cmp r0,#0xd5; bls L; b default; L: lsls..; ldr..; mov pc,r0`): o limite nao e levado pelo
  desvio condicional, `last_cmp` chega `None` e cai no teto de 64.
- Todas as entradas extras apontam para dentro da propria funcao (validado). Total: 5 alvos
  distintos novos. Correcao: no analisador, usar o `cmp #imm` do limite em vez de capar em 64.
**2. Travada ao minimizar.** `hw/window_sdl.c` cria o renderer com `SDL_RENDERER_PRESENTVSYNC`
  e chama `SDL_RenderPresent` todo quadro; com a janela minimizada o present bloqueia, e como
  o ritmo do jogo segue a fila de audio, a fila esvazia (underruns seguidos nos quadros
  3776-3788 do log). Correcao: nao apresentar quando minimizada e dispensar o vsync (o relogio
  do audio ja pace o jogo).
**3. Mensagem enganosa.** O contexto do acesso a 0x100F0D55 sai como `PARADA`, mas e so aviso.

## Teste de estresse com entrada aleatoria (Riven) - `tools/fuzz_run.py`
`python tools/fuzz_run.py --seeds 1-124 --frames 9000 --jobs 6` roda `rt_boot.exe` sem janela e sem
save, com um roteiro de teclas aleatorio por semente (prologo fixo ate a 1a fase + caminhada com
direita dominante, A/B, L/R, START/SELECT). Uma execucao e OK se termina em `RT_STOP frames`, sai
com codigo 0 e tem `nao_resolvidos=0`. Falhas guardam o log completo em `build/fuzz/seed_N.log`.
- **Resultado (mapa com 5671 funcoes, tabelas de salto corrigidas): 124 de 124 limpas.**
- **Controle negativo:** a mesma semente com `--no-sram-alias` da `nao_resolvidos=70` e o runner
  a classifica como falha; logo o detector funciona.
- **Limite honesto do que isto cobre:** `--keys` do runtime aceita <= 128 eventos e <= 1023 caracteres,
  entao cada roteiro tem ~83 eventos e a entrada acaba por volta do quadro ~5100; o resto e
  parado. Sao ~50 s de jogo aleatorio por execucao, ~100 min no total, **todos na area inicial**
  (conferido em quadros: Kirby fica perto do inicio da 1a fase). Nao ha profundidade: fases
  posteriores, chefes, outros poderes e ajudantes nao foram exercitados.
- Para ir alem (pedido ao Nasus): `--fuzz SEMENTE` dentro do runtime (sem limite de roteiro) e/ou
  `--keys-file`; e uma forma de partir de um save com progresso (o `baserom.sav` do jogador).

## Distribuicao: "abrir um exe e arrastar a ROM" (Riven) - plano
**Hoje:** `python -m recomp.program --window 3` gera `build/recomp/program/kirby.exe` (+ `SDL2.dll`),
~26 MB. Ele **contem o jogo recompilado** e le a ROM em tempo de execucao (dados de graficos e
audio): precisa do `baserom.gba` ao lado, e depende de Python+zig+SDL2 so para ser *gerado*.
**Regra legal que decide a arquitetura:** o `kirby.exe` e codigo do jogo traduzido. **Nao pode ser
distribuido**; tem que ser gerado na maquina de cada usuario, a partir da ROM dele. O que se
distribui e so o *lancador + compilador + runtime* (sem nenhum byte do jogo).

### Fluxo desejado
1. Usuario baixa `KirbyRecomp.zip` (nosso codigo, zig portatil, SDL2; sem ROM nem codigo gerado).
2. Arrasta a ROM sobre `KirbyRecomp.exe` (ou abre e escolhe/solta o arquivo).
3. 1a vez: verifica SHA-1 (so a versao USA/B8KE), roda analise -> recompila -> compila -> abre o
   jogo. **Minutos** (ver medida abaixo). Cache em `%LOCALAPPDATA%\KirbyRecomp\<sha1>\`.
4. Proximas vezes: abre direto (cache valido enquanto a versao do lancador/runtime nao mudar).
5. Save em `<nome da rom>.sav` ao lado da ROM.

### Pecas e donos sugeridos
| Peca | O que | Dono |
|---|---|---|
| `tools/build_all.py` | ROM -> build completo em um comando (pronto; ver medida) | Riven |
| Launcher + empacotamento | `launcher.py` (arrastar/escolher ROM, hash, cache, progresso), PyInstaller, zip de release, guarda "nada do jogo no zip" | Riven |
| Caminhos relativos | `program.py`/`harness.py`: sem `C:/libs`, sem `.venv`, `OUT` configuravel (cache), `--rom` sem exigir cwd do repo; SDL2/zig localizados ao lado do exe | Nasus |
| `kirby.exe` sem argumentos | duplo clique = procurar `baserom.gba`/ultima ROM, escala padrao, F11 tela cheia, soltar ROM na janela (SDL_DROPFILE) | Nasus + Gwen |
| Icone, titulo, configuracoes | escala, mudo, tela cheia, persistencia em arquivo ini | Gwen |
| Teste de instalacao limpa | maquina/pasta sem Python, sem `.venv`, sem repo | Riven |

### Riscos
1. **Tamanho:** zig portatil (~80 MB) + Python embutido + SDL2: release de ~120-150 MB.
2. **Tempo da 1a execucao** em PCs mais lentos; precisa de barra de progresso e de nunca parecer travado.
   Alternativa futura: iniciar ja com o interpretador (rt_interp) enquanto compila em segundo plano.
3. **Antivirus** (exe do PyInstaller nao assinado; compilador gerando e executando exe).
4. **Caminhos**: usuario com espacos/acentos em `Documents`; AppData; permissoes.
5. **Versoes da ROM**: so a USA (SHA-1 acima). Outras regioes precisam de mapa proprio.
6. **Legal**: o zip e a pasta de cache nunca devem conter codigo gerado em release; CI deve falhar
   se `build/` ou `*.gba` entrarem no pacote.

### Medida: construcao do zero (Riven) - `tools/build_all.py` em copia limpa
Copia de `git archive HEAD` + so a ROM, sem `build/`, 8 jobs, mesma maquina de desenvolvimento:
| Etapa | Tempo |
|---|---|
| analise (pass 1) + indiretos + confianca + analise (pass 2) | 20 s + 5 s + 6 s + 20 s = **~51 s** |
| recompilar + compilar + convergir (`recomp.program --frames 700`) | **973 s (16 min)** |
| **Total** | **17 min** |
- O ultimo `compilado em 182s` e so a ultima rodada; ha varias (cada rodada do `iterate` que acha
  funcao nova recompila shards). A causa de rodadas extras: **13 handlers do M4A** e outras
  funcoes so eram descobertos executando (`[unresolved] +0814F958 ...`), porque na minha arvore
  eu os tinha injetado a mao em `build/indirect/m4a_handlers.csv` (nao versionado).
- **Correcao (`resolve_indirect.py`, passo 4b "completar tabelas de ponteiros")**: uma sequencia de
  >= 5 ponteiros Thumb consecutivos para a area de codigo, com metade ja conhecida, e uma tabela de
  despacho; as demais entradas entram se decodificam ate um retorno. Sem lista manual: 37 funcoes,
  **16 das 17 do M4A** (a 17a, 0x0814FFC0, e `bl` de 0x0814FF64 e o analisador a acha sozinho) e
  outras 20, entre elas **0x0802FC14**, que o runtime so tinha achado quando o jogo passou do titulo.
- Pendente: medir de novo a construcao do zero com isso (esperado: menos rodadas do `iterate`).
- Onde esta o tempo: cada rodada recompila shards; 16 shards x ~600 mil instrucoes em C. Opcoes:
  `-O1`/`-Os`, menos C por instrucao, so o fecho alcancavel, e iniciar interpretando.
