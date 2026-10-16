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
- [x] Licenca MIT (`LICENSE`); listagens da ROM removidas deste arquivo (so descricao + exemplo proprio)

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

### O que a ROM faz no arranque (descrito, sem copiar o codigo)
O codigo de inicializacao (`crt0`, em `0x080000C0`, ARM) faz o que quase todo jogo de GBA faz:
1. entra no modo **IRQ**, define a pilha dessa modalidade (IWRAM `0x03007FA0`);
2. entra no modo **System** e define a pilha normal (`0x03007E80`);
3. grava o endereco do tratador de interrupcoes (`0x08000100`) no vetor de IRQ da IWRAM (`0x03007FFC`);
4. chama o `main` com um `bx` **ARM -> Thumb** (`0x08152A04`, Thumb) e, se ele retornar, recomeca.
O tratador de IRQ em `0x08000100` le **IE e IF** do bloco de I/O (`0x04000200`), calcula `IE & IF`
(interrupcoes pendentes e habilitadas) e salta por uma tabela em IWRAM (`0x030017B0`), um endereco por fonte.
Um `main` Thumb tipico de agbcc abre com `push {lr}`, chama sub-rotinas com `bl` (par de halfwords)
e retorna com `pop {r0}; bx r0` (retorno "interworking").
*Quem quiser ver as instrucoes reais pode desmontar a propria ROM nesses enderecos (por exemplo,
`python tools/romhdr.py baserom.gba`); este texto nao reproduz a listagem do jogo.*

### Exemplo didatico (escrito para este texto, nao e da ROM)
```
@ ARM -> Thumb e volta: o bit 0 do endereco escolhe o modo
        ldr   r1, =rotina_thumb + 1      @ +1 => bit 0 = 1 => Thumb
        mov   lr, pc                     @ PC ja vale "esta instrucao + 8" em ARM
        bx    r1                         @ salta e troca de modo

rotina_thumb:                            @ 16 bits por instrucao
        push  {r4, lr}
        bl    outra_funcao               @ em Thumb o BL e um par de halfwords
        pop   {r4}
        pop   {r0}
        bx    r0                         @ retorno 'interworking' (volta para ARM ou Thumb)
```
Note o "operando 2 com shift" do ARM (`and r1, r2, r2, lsr #16`) e a condicional (`ne`, `eq`...) em
quase todas as instrucoes ARM; em Thumb, condicoes so em branches.

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

### QA do launcher e do controle (Poppy) - testes automatizados e roteiro manual
**Automatizado:** `python -m unittest tests.test_launcher_qa -v` (raiz do repo; ~35 s; 80 testes; sem SDL2 dev os de teclado/controle/retangulo PULAM). Compila de
verdade, num harness C minimo, o `hw/config_ini.c` (UI) e o `recomp/runtime/rt_input.c` (jogo, sem SDL), e
le o `launcher/flow.py`. Sem compilador funcional os testes de C pulam com o motivo (zig do projeto, `.venv`,
WinLibs ou gcc/clang do PATH). Cobertura:
| grupo | o que garante |
|---|---|
| v1 antigo | chaves soltas/`[kirby]`, valores invalidos -> padrao, comentarios `;`/`#`, CRLF, caixa, duplicata (ultima vence), arquivo vazio/binario/linha >255 bytes, `cfg_path_beside` (barras, acentos, sem diretorio) |
| v2 contrato | todas as chaves de `[video] [audio] [keyboard] [controller] [launcher]` com padrao e faixa, faltando secao/chave, secao desconhecida, reservadas (Esc/F11/F1), binding duplicado reseta o teclado, nomes longos sem estouro, migracao v1 -> v2, round-trip estavel |
| mesmo arquivo | launcher.py (`ini_path = dirname(rom)/kirby.ini`) == jogo (`kirby.ini` ao lado de `<rom>.sav`, rt_main.c); jogo carrega o ini e aceita `--ini`; F1 = codigo 77 no launcher |
| paridade dos 3 leitores | UI (`config_ini.c`) x jogo (`rt_cfg_load`) x launcher (`flow.read_ini_value`) dao os mesmos valores em 14 arquivos comuns |
**Achados (cada um foi um `expectedFailure` com o arquivo de entrada; corrigido o teste vira
"unexpected success" e o decorador sai). Estado em 2026-10: 1-4 e 6 CORRIGIDOS (o jogo passou a usar o
mesmo parser de `hw/config_ini.c`; `flow.py` ja corta `#`); o 5 (BOM) tambem foi corrigido. Hoje so restam os 2 `expectedFailure` de nomes SDL invalidos (secao abaixo):**
1. `rt_input.c` `to_int`: `scale=2x` vale 2 no jogo, 3 (padrao) na UI. Dono: Nasus.
2. `rt_input.c`: `deadzone` maximo 32000 (UI/contrato: 32767). Nasus.
3. `rt_input.c`: `a=Escape` deixa o botao A SEM tecla no jogo e volta a X na UI; binding duplicado
   mantem as duas (uma tecla = 2 botoes) enquanto a UI reseta o teclado. Nasus/Gwen.
4. `launcher/flow.py` `read_ini_value`: so corta `;`; `show=0 # x` abre o launcher, o C le 0. Riven.
5. `hw/config_ini.c`: BOM UTF-8 no inicio faz a 1a chave virar `\xef\xbb\xbfscale` (ignorada). Gwen.
6. Causa de 1-4: o jogo tem parser PROPRIO. `hw/config_ini.c` ja nao usa SDL: usar a mesma funcao no
   `kirby.exe` elimina as divergencias (`test_game_uses_the_shared_parser`).
**Mensagens em ingles (build.log):** as mensagens de `tools/analyze_rom.py` e `tools/check_map.py`
(prints, `--help`, erros) foram traduzidas; as dos demais `tools/*.py` ja estavam em ingles (varredura
por palavras portuguesas em print/exit/raise: 0 ocorrencias). Comentarios continuam em portugues.

**Roteiro manual (marcar cada item; build limpo com `KirbyRecomp.exe`; logs em
`%LOCALAPPDATA%\KirbyRecomp\launcher.log` e `build.log`):**
Preparo: uma pasta SEM `kirby.ini`; ROM valida `baserom.gba`; outra pasta com um `.gba` qualquer (copia
renomeada de um arquivo de texto) para o caso invalido; um controle USB/Bluetooth (Xbox ou Switch Pro).
| # | Passo | Resultado esperado |
|---|---|---|
| 1 | Abrir `KirbyRecomp.exe` sem argumentos, sem `kirby.ini` | Tela do launcher aparece (nao abre o jogo direto). Titulo `KIRBY RECOMP`, ROM mostra `ok`, status em ingles (`Preparing...` -> `Building NN% (you can play now)` -> `Compiled version ready`). Nenhum texto em portugues, nenhum acento quebrado |
| 2 | Janela: redimensionar, minimizar, restaurar, arrastar entre monitores | Sem travar; ao restaurar, volta a atualizar (progresso continua) |
| 3 | Escolher uma ROM invalida (arquivo que nao e GBA / SHA-1 diferente) | Mensagem `This ROM is not supported.` com o SHA-1 e a versao aceita; launcher nao inicia build; pode escolher outra |
| 4 | ROM inexistente (arrastar e apagar antes / caminho errado) | `File not found: <caminho>`; sem excecao/traceback |
| 5 | Construcao em andamento: abrir, esperar `Preparing fast mode`, apertar PLAY quando habilitar | Jogo abre pelo interpretador; barra continua no launcher/`build.log`; ao terminar, status `Compiled version ready`; nada trava |
| 6 | Construcao em andamento: fechar a janela de progresso | Pergunta de confirmacao (`Cancel building the game?`); `No` mantem a construcao; `Yes` cancela e nao deixa processo orfao (conferir no Gerenciador de Tarefas) |
| 7 | Jogando: F1 | Jogo fecha com codigo 77 e o launcher reaparece com as mesmas configuracoes; PLAY reabre o jogo |
| 8 | Jogando: Esc | Sai do jogo (e do launcher). F11 alterna tela cheia e NAO fecha |
| 9 | Remapear teclado: Keyboard... -> clicar em A -> tecla Q | Mostra `Press a key for A...`; Esc cancela sem mudar; Q fica gravada em `[keyboard] a=Q`; jogo usa Q como A |
| 10 | Remapear para tecla ja usada | Pede troca/conflito (nao deixa duas teclas no mesmo botao); `Reset to defaults` restaura X/Z/A/S/Return/Backspace/setas |
| 11 | Tentar atribuir Esc, F1 ou F11 | Recusado com aviso de tecla reservada; atribuicao anterior preservada |
| 12 | Controle conectado ANTES de abrir | Aba Controller lista/usa o controle; navegar a tela so com o controle (D-pad + A/B); botoes por POSICAO (A do GBA = botao leste) |
| 13 | Conectar o controle COM o jogo aberto; depois desconectar | Passa a responder sem reiniciar; ao desconectar, nada trava e o teclado continua valendo |
| 14 | Direcional analogico: `stick_as_dpad=1` e `0`; zona morta | Com 1 o analogico move; com 0 so o D-pad; pequenas inclinacoes (< zona morta) nao movem |
| 15 | Escala 1x..6x e `Integer scaling` | Janela = 240x160 x N; com inteira, sem barras tortas/pixels borrados; guardado em `[video]` |
| 16 | Tela cheia (checkbox e F11) | Entra/sai sem perder o audio; restaura a escala anterior ao sair; persiste em `fullscreen=` |
| 17 | Volume e Mute | Barra altera o volume do jogo; Mute silencia; persistem em `[audio]` |
| 18 | Arquivo `kirby.ini` editado a mao com lixo (`scale=abc`, chave desconhecida, BOM) | Abre normalmente com os padroes para o que for invalido (BOM e o achado 5: confirmar o comportamento) |
| 19 | `Show launcher` desmarcado; abrir de novo; segurar Shift | Sem Shift: abre o jogo direto; com Shift: mostra o launcher |
| 20 | Pasta com espacos/acentos (`C:\Users\Joao Silva\Documentos`) e disco sem permissao de escrita | Funciona; sem permissao: mensagem clara em ingles (nao fecha em silencio) |
| 21 | Launcher: trocar `Window size` (1x..6x) com o launcher aberto | A janela do LAUNCHER muda de tamanho para `240*N x 160*N` (minimo 480x320); em 1x a UI aparece em 2x com a legenda `1X PREVIEW (UI AT 2X)`; o jogo, ao abrir, usa o mesmo N |
| 22 | Launcher: marcar `Fullscreen` | `Window size` fica DESABILITADO (cinza, nao clica); desmarcar reabilita e restaura o N anterior; com fullscreen a UI preenche a tela |
| 23 | Launcher: `Select ROM` com e sem ROM carregada | Sem ROM: botao `Select ROM` visivel. Com ROM valida carregada: o botao some e aparece so `ROM: <nome> OK`; ROM invalida volta a mostrar o botao |
| 24 | Escala do jogo: `scaling=integer`, `fit`, `stretch` em tela cheia 1920x1080 | integer = 1440x960 com bordas nos 2 eixos; fit = 1620x1080 (bordas so nas laterais, 3:2); stretch = 1920x1080 (preenche e distorce). Sem pixels borrados em integer |
| 25 | **[BLOQUEIA]** Remapear CADA botao do teclado (A, B, L, R, Start, Select, Up, Down, Left, Right) para uma tecla livre, um de cada vez | Cada um mostra `Press a key for <X>...`; a tecla nova grava em `[keyboard]`; no jogo SO o botao remapeado responde a nova tecla e a antiga para de valer; as demais ficam como estavam |
| 26 | **[BLOQUEIA]** Remapear CADA botao do controle (A, B, L, R, Start, Select, D-pad x4) | Idem com `Press a button for <X>...`; grava em `[controller]`; vale no jogo e na navegacao do launcher |
| 27 | **[BLOQUEIA]** Conflito: atribuir uma tecla/botao que ja e de OUTRO botao | Pede troca ou recusa (nunca deixa dois botoes na mesma tecla); apos confirmar, os dois ficam coerentes; o arquivo nao tem duplicatas (teclado e controle sao independentes) |
| 28 | **[BLOQUEIA]** `Reset to defaults` (teclado e controle) | Volta a X/Z/A/S/Return/Backspace/setas e b/a/leftshoulder/rightshoulder/start/back/dpad; `stick_as_dpad=1` e `deadzone=8000`; arquivo gravado com os padroes |
| 29 | **[BLOQUEIA]** Reservadas: tentar Esc, F11, F1 em qualquer botao (teclado) | Recusa com aviso; a tecla anterior continua; Esc ainda sai, F11 ainda alterna tela cheia, F1 ainda volta ao launcher |
| 30 | **[BLOQUEIA]** Desconectar o controle NO MEIO do jogo (segurando uma direcao pelo analogico) | A direcao SOLTA na hora (Kirby para de andar); o teclado segue funcionando; sem travar nem mensagem de erro; reconectar volta a funcionar sem reiniciar (ate ~0,5 s) |
| 31 | **[BLOQUEIA]** F1 durante o jogo -> launcher -> Play | Launcher volta com as MESMAS teclas/botoes (conferir nas abas Keyboard/Controller); Play reabre o jogo e as teclas remapeadas continuam valendo; nada foi regravado com padroes |
| 32 | Remapear com o jogo ja fechado e abrir de novo; editar o `kirby.ini` a mao com nome SDL invalido (`a=Foo`) | Tecla valida vale; nome invalido: ver achado de nomes invalidos (hoje deixa o botao sem tecla: confirmar e reportar) |
| 33 | Analogico: movimento suave, diagonais, parar no centro; ajustar `deadzone` (0, padrao 8000, 32767) | 8 direcoes por combinacao de eixos; sem tremida na borda (histerese 85%); deadzone 0 reage a qualquer toque; 32767 so deflexao total; `Left stick as D-pad` desmarcado: analogico ignorado e D-pad continua |
| 34 | **[BLOQUEIA]** Salvar no jogo pela 1a vez | `<nome da ROM>.sav` aparece ao lado da ROM, com EXATAMENTE 32768 bytes; nao fica `.sav.tmp` |
| 35 | **[BLOQUEIA]** Salvar no jogo, esperar ~6 s e matar o `kirby.exe` pelo Gerenciador de Tarefas (Finalizar tarefa) | Ao reabrir, o progresso esta la (debounce 1 s, teto 5 s). Matar em MENOS de 1 s pode perder essa ultima escrita (aceitavel), mas NUNCA corromper |
| 36 | **[BLOQUEIA]** Salvar e sair de 4 formas: Esc, fechar a janela (X / Alt+F4), F1 (volta ao launcher), logoff/desligar com o jogo aberto | Em todas o progresso esta gravado ao reabrir |
| 37 | **[BLOQUEIA]** Pasta da ROM somente-leitura ou disco cheio durante o jogo | O jogo NAO trava; 1 aviso em `kirby.log` (`[save] could not write ... will retry`), nao repete a cada tentativa; liberado o espaco/permissao, aparece `write recovered` e o save e gravado |
| 38 | **[BLOQUEIA]** Backups automaticos: jogar, salvar, sair, abrir o launcher e dar Play | Surge `%LOCALAPPDATA%\KirbyRecomp\Backups\<sha1 da ROM>\<nome>_AAAAMMDD_HHMMSS.sav` com o save ANTERIOR ao Play; save inalterado -> nenhum backup novo; depois de 11+ saves diferentes ficam so os 10 mais recentes |
| 39 | **[BLOQUEIA]** `OPEN FOLDER` | Abre o Explorer na pasta do `.sav`; sem ROM carregada o botao fica desabilitado |
| 40 | **[BLOQUEIA]** `EXPORT...` | Janela de salvar arquivo; o `.sav` exportado tem o MESMO hash (SHA-1) do original; cancelar nao faz nada; sobrescrever um arquivo existente pede confirmacao do Windows |
| 41 | **[BLOQUEIA]** `IMPORT...` com arquivo valido | Pergunta `OVERWRITE CURRENT SAVE? A BACKUP IS KEPT` (padrao NAO); NAO/Esc nao muda nada; SIM substitui o save e o save antigo aparece em Backups; Play abre com o progresso importado |
| 42 | **[BLOQUEIA]** `IMPORT...` com arquivo de tamanho errado (0 bytes, 1 KB, 64 KB, um `.png`) | Recusado com mensagem em ingles dizendo o motivo/tamanho esperado (32768); save atual intacto; nenhum backup criado |
| 43 | `IMPORT...` de um save "em branco" (32768 bytes todos 0xFF ou 0x00) | Aviso explicito (save vazio) antes de sobrescrever; o backup do atual e feito |
| 44 | Round trip: EXPORT, jogar mais, IMPORT o exportado | O progresso volta EXATAMENTE ao do export; o progresso descartado fica no backup mais recente |
| 45 | **[BLOQUEIA]** Pasta da ROM com acento (ex.: `Joao` com til), espaco e caracteres nao-latinos (japones/cirilico) | Salvar, backup, abrir pasta, exportar e importar funcionam; o `.sav` fica ao lado da ROM (nunca em outro lugar nem com `?` no nome) |
| 46 | Botoes SAVE sem ROM carregada ou com ROM invalida | OPEN FOLDER/EXPORT/IMPORT desabilitados (cinza); nenhum aceita clique |
| 47 | **[BLOQUEIA]** F1 durante o jogo -> IMPORT -> Play | O jogo abre com o save IMPORTADO e nao o antigo (o gravador nao pode sobrescrever o arquivo importado com a SRAM velha: o jogo ja saiu e gravou ANTES do import) |
Aceite: itens 1-8, 12-13, 21-23, 25-31 e 34-42, 45, 47 (marcados **[BLOQUEIA]**) sao obrigatorios para liberar; os
demais, sem regressao em relacao a versao anterior.

### QA do gerenciamento de save (Poppy) - `tests/test_save_qa.py`
`python -m unittest tests.test_save_qa -v` (~3 s). Complementa o `tests/test_save.py` do Nasus (autoteste com relogio
falso + kills aleatorios do `rt_boot.exe`, que precisa do build): aqui o `recomp/runtime/rt_save.c` REAL e compilado
num harness (relogio falso, SRAM simulada, `fwrite` interceptado), entao roda sem ROM nem build.
**Runtime (`RuntimeSaveTest`, 16 testes, todos OK):**
| contrato | verificado |
|---|---|
| arquivo novo / existente | 1o flush cria 32768 bytes de 0xFF; ida e volta byte a byte; arquivo curto e preenchido com 0xFF, maior e cortado em 32 KiB |
| debounce ~1 s | 0 flush em +0 e +0,5 s; 1 flush em +1,1 s; limpo nunca regrava (mtime igual em 39 s); escrita do mesmo valor nao suja |
| rajada | 100 escritas em 200 ms = 1 flush so, com o conteudo final |
| teto | escrevendo todo quadro por 12 s: 2 a 3 flushes (no maximo 5 s de atraso) |
| saida | `exit` normal grava sem precisar de tick (atexit); `kill -9` (`_exit`) antes do debounce deixa o save antigo INTEIRO, depois do debounce o novo esta em disco |
| atomicidade | `fwrite` do `.tmp` interrompido (morre) apos 0, 1, 100, 16384 e 32767 bytes: o `.sav` e sempre o antigo ou o novo inteiro, 32768 bytes; a proxima execucao grava normal e nao sobra `.tmp`; primeira gravacao morta: sem `.sav` ou inteiro |
| falha + nova tentativa | `.tmp` impossivel de criar (diretorio no lugar): `.sav` intacto, tenta a cada ciclo (>= 3 falhas em 5 s), 1 unico aviso `could not write` por sequencia, recupera quando liberado |
| caminhos | acento, espaco, parenteses, e nao-ANSI (japones, cirilico): grava no lugar certo (usa UTF-8 -> wide) |
| autoteste do proprio runtime (`rt_selftest_save`) | passa |
Resultado: o gravador novo corrige as 3 falhas do antigo (`fopen("wb")` truncava antes de escrever; so gravava no exit; caminho
fora da pagina ANSI virava `?`). Limite conhecido e aceito: `kill -9` perde ate ~1 s (ate 5 s sob escrita continua) de progresso.
**Launcher (`LauncherSaveContractTest`, 15 testes): PULAM ate existir `launcher/savemgr.py`** (Riven). Interface de teste
(injeta pasta de backups e relogio; qualquer outra assinatura e so avisar que o teste se adapta):
```
backup_save(sav_path, backup_root, rom_sha1, rom_name, now=None) -> str | None   # None se igual ao backup mais recente
import_save(src_path, sav_path, backup_root, rom_sha1, rom_name, now=None) -> (ok: bool, message: str)
export_save(sav_path, dst_path) -> None
# backups: <backup_root>/<rom_sha1>/<rom_name>_AAAAMMDD_HHMMSS.sav ; rotacao automatica (10 mais recentes)
```
Ja escritos (contra o contrato do Riven): backup so quando o save mudou; nome no padrao e pasta por SHA-1; rotacao em 10
removendo os mais antigos; dois saves diferentes no MESMO segundo nao se sobrescrevem; backups separados por ROM; sem
`.sav` = nada a fazer; import recusa 0/1/100/32767/32769/65536/131072 bytes SEM tocar no save nem criar backup e com
mensagem; save "em branco" (0xFF/0x00) avisa; import faz o backup do ATUAL antes e troca por rename (sem `.tmp`); import de
arquivo inexistente da erro; falha na troca (os.replace quebrado) deixa o original intacto; export nao altera o save e
sobrescreve o destino; round trip exportar -> importar identico (SHA-1) com acento/espaco nos DOIS caminhos.
O oraculo desses testes e o `SaveModel` (validacao, nomes, rotacao) que roda sempre em `ModelSpecTest`.
**Pendencias:** Riven entrega `savemgr.py` (ou diz os nomes que usou); Gwen: a UI ja devolve KL_OPEN_SAVE_DIR/EXPORT/IMPORT
(botoes desabilitados sem ROM valida, confirmacao dentro da UI); roteiro manual 34-47 acima.

### Teclas, botoes e analogico: contrato e testes (Poppy)
Os testes rodam o `rt_input_init()`/`rt_input_poll()` REAIS (`recomp/runtime/rt_input.c`, RT_WINDOW) com uma camada
SDL falsa por macro (controle, eixos, botoes e teclado controlaveis por script) e SDL2 de verdade para resolver nomes
(`SDL_GetKeyFromName`, `SDL_GameControllerGetButtonFromString`). Classes em `tests/test_launcher_qa.py`:
`KeyboardControllerBindingTest` (16) e `AnalogStickTest` (14).
**Contrato verificado ([keyboard]/[controller]):**
| regra | resultado |
|---|---|
| todos os nomes padrao existem no SDL (teclas e botoes) | ok |
| arquivo vazio / v1 (`scale=`/`[kirby]`) -> teclas e botoes padrao | ok |
| nomes validos (`Q`, `q`, `Space`, `Left Shift`, `Keypad 5`, `F5`...; `x y guide leftstick rightstick`) | ok, 1 botao muda, os outros ficam |
| maiusculas/minusculas (`RETURN`, `LeftShoulder`) | ok |
| reservadas `Escape`/`F11`/`F1` (qualquer caixa), em qualquer botao | volta a tecla padrao daquele botao; nunca fica ligada |
| duplicata no teclado (`a=Z` com B=Z) | o mapa do TECLADO inteiro volta ao padrao; controle intacto |
| duplicata no controle (`a=back` com SELECT=back) | o mapa do CONTROLE inteiro volta ao padrao; teclado intacto |
| teclado e controle acionam o mesmo bit do KEYINPUT; teclado segue com o controle desconectado | ok |
| UI (`config_ini.c` -> SDL) == jogo (efetivo) em 8 arquivos | ok |
| launcher: nao le teclas/botoes (so `show`/`scaling`); nunca regrava os padroes | teste PULA ate existir `flow.read_bindings` |
**Achados abertos (2 `expectedFailure`, dono: Nasus, `rt_input.c` `rt_input_init`):** nome que o SDL NAO conhece
(`a=NotAKey`, `a=Ctrl+Z`; controle: `a=banana`) deixa o botao SEM tecla/botao em vez de voltar ao padrao
("invalid value falls back to its default"). `config_ini.c` so valida a sintaxe do nome; o SDL so e consultado no
jogo. Sugestao: se `SDL_GetKeyFromName` falhar, usar o nome padrao do botao (e logar uma vez).
**Analogico (`rt_input_poll`; so o analogico ESQUERDO; `hyst`):**
| caso | resultado |
|---|---|
| abaixo da zona morta (`|v| < dz`, 8 combinacoes) | nada |
| `|v| == dz` ou maior | direcao aperta (limiar inclusivo) |
| diagonais (+-20000,+-20000) | 2 bits (ex. RIGHT+DOWN); eixo dentro da zona morta so o outro |
| histerese | aperta em `>= dz`; so solta abaixo de 85% de `dz` (8000: segura em 7000 e 6800, solta em 6799; volta a apertar so em 8000); sem tremida em 9000/7900/8100/7900... |
| `stick_as_dpad=0` | analogico ignorado; D-pad (botoes) continua |
| analogico direita + D-pad esquerda | cancelam o par (regra do GBA) |
| `deadzone=0` | repouso (0,0) nada; +-1 ja aperta (dz efetivo minimo 1) |
| `deadzone=32767` | 32766 nada; +32767 e -32768 apertam (simetrico, inclusive o minimo do int16) |
| `deadzone` fora de 0..32767 / lixo no arquivo | volta a 8000 |
| controle desconectado no meio do movimento | direcao SOLTA no mesmo quadro; reconecta em ate 30 quadros e volta a valer |
Notas de desenho confirmadas no codigo: so um controle ativo por vez (os outros sao ignorados, `manage_pad`);
varredura de hot-plug a cada 30 quadros (~0,5 s) para CONECTAR; na desconexao a direcao solta no mesmo
quadro (no teste o SDL falso devolve 0 para controle removido, como o SDL real: confirmar com o item 30 do roteiro).

### Escala de video: `[video] scaling=integer|fit|stretch` - contrato, referencia e testes (Poppy)
Contrato (`hw/config_ini.h`; decisao do usuario): padrao **fit**. `integer_scale=1` (v1/v2 antigo) -> integer,
`integer_scale=0` -> fit; um `scaling=` valido SEMPRE vence; invalido -> padrao (a chave antiga, se valida,
ainda vale). O arquivo gravado nao tem mais `integer_scale`.
Retangulo de destino na janela `W x H` (referencia, so inteiros):
- integer: `k = max(1, min(W//240, H//160))`; `240k x 160k`, centralizado (`(W-w)//2`, `(H-h)//2`)
- fit: `k = min(W/240, H/160)` real; `floor(240k) x floor(160k)`, centralizado (3:2; barra so nas laterais num 16:9)
- stretch: `(0, 0, W, H)`
Medido no SDL real (`rt_apply_scaling` + `dest_rect` de `recomp/runtime/rt_window.c`, driver `dummy`); x,y,w,h:
| janela | integer | fit | stretch |
|---|---|---|---|
| 1920x1080 | 240,60 1440x960 | 150,0 1620x1080 | 0,0 1920x1080 |
| 1366x768 | 203,64 960x640 | 107,0 1152x768 | 0,0 1366x768 |
| 2560x1440 | 200,0 2160x1440 | 200,0 2160x1440 (= integer, 9x exato) | 0,0 2560x1440 |
| 1280x720 | 160,40 960x640 | 100,0 1080x720 | 0,0 1280x720 |
O SDL bate com a referencia nos 12 casos, sem diferenca de 1 px (a tolerancia do teste e 1 px).
Testes (`tests/test_launcher_qa.py`):
| classe | o que faz | estado |
|---|---|---|
| `ScalingReferenceSpecTest` | a referencia reproduz a tabela feita a mao + invariantes (dentro da janela, centrado +-1 px, multiplos de 240x160 no integer, 3:2 no fit) | roda sempre |
| `ScalingParseUITest` | `scaling=` no `config_ini.c`: padrao fit, valores validos (qualquer caixa), invalidos -> fit, legado `integer_scale`, `scaling` vence, round-trip | ativo |
| `ScalingParseParityTest` | jogo (`rt_cfg.scaling`, via `rt_input.c`) e launcher (`flow.read_scaling`) = UI em 8 arquivos | jogo ativo; launcher PULA ate existir `flow.read_scaling(ini_path)` |
| `GameDestRectTest` | compila o `rt_apply_scaling`/`dest_rect` reais com o SDL dummy e compara 4 resolucoes x 3 modos | ativo (precisa SDL2 dev: `SDL2_DIR` ou `C:/libs/SDL2-2.32.2`) |
| `UIScalingModeTest` | `launcher_ui.c` expoe os 3 modos | PULA: a UI ainda so tem o checkbox `INTEGER SCALING` |
Pendencias para fechar a comparacao UI x jogo: (1) UI: seletor dos 3 modos em `launcher_ui.c` (Gwen);
(2) launcher: `flow.read_scaling(ini_path)` devolvendo `integer|fit|stretch` com a mesma regra legada (Riven).
Hoje a UI nao desenha um retangulo de jogo proprio (so a janela `SetLogicalSize(KL_W, KL_H)`), entao nao ha
segundo calculo para comparar com o do jogo; quando o preview existir, adicione-o em `GameDestRectTest`.

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

### 12. Launcher UI (kirby_ui.dll) and kirby.ini v2 - phases 0 and 1 (Gwen)
Contract: NOTES 'Launcher completo: tela de configuracao e botao Play (Riven)'. All UI text is English.
- **`hw/config_ini.[ch]` (kirby.ini v2, no SDL):** sections `[video] scale fullscreen integer_scale vsync`,
  `[audio] volume mute`, `[keyboard] a b l r start select up down left right` (SDL key names),
  `[controller]` same buttons (SDL game-controller names) + `stick_as_dpad deadzone`, `[launcher] show language`.
  `cfg_defaults / cfg_load / cfg_save / cfg_validate / cfg_reset_keyboard / cfg_reset_controller /
  cfg_path_beside / cfg_equal / cfg_key_reserved / cfg_button_name`. Defaults are the contract's (keyboard BY
  POSITION: A=X, B=Z, L=A, R=S, Start=Return, Select=Backspace, arrows; controller GBA A = SDL `b` (east),
  GBA B = SDL `a`). Validation: out-of-range number -> default, empty/illegal name (`= ; # [ ]`, control chars,
  >31 chars) -> default, **Escape/F11/F1 never bindable** (any case), duplicate bindings reset the whole map
  (the stick/deadzone are kept). Keys are case-insensitive, `;`/`#` comments, unknown keys/sections ignored.
  **v1 compatible:** flat file or `[kirby]` section with scale/mute/fullscreen/vsync loads; the v1 field names
  and order of `KirbyConfig` are unchanged (`KirbyConfig c = {5,1,1,1}` still means scale/mute/fullscreen/vsync);
  a v1 file is upgraded to v2 on the next `cfg_save`.
- **`hw/launcher_ui.[ch]` -> `kirby_ui.dll`:** `int kl_run(const char *ini_path, KlPoll poll)` returns `KL_PLAY=1`,
  `KL_QUIT=0` or `KL_PICK_ROM=2`; `KlStatus {state, percent, text[96], rom[260], rom_ok}`; `poll` is called ~10x/s
  on the UI thread (default 100 ms; `kl_set_poll_interval`). Extra export: `const char *kl_dropped_path(void)`
  (a file dropped on the window returns `KL_PICK_ROM` and the path is here). Screen (640x420 logical, resizable,
  icon + `WIN_TITLE`): header (KIRBY RECOMP, ROM name + OK / (unverified) / none, SELECT ROM...), status panel
  with text + progress bar (states 1 and 2), VIDEO (window size 1X-6X radio, Fullscreen, Integer scaling),
  AUDIO (volume slider 0-100, Mute), INPUT (KEYBOARD..., CONTROLLER..., RESET TO DEFAULTS), PLAY, QUIT.
  **PLAY is disabled when `state == 1`**; with `state == 0` (no ROM) it returns `KL_PICK_ROM`.
  Every change is written to the ini immediately. Mouse (click, drag the slider, hover), keyboard (Tab /
  Shift+Tab / Up / Down move the focus; Left/Right change radios and the slider by 5; Enter/Space activate;
  Esc quits or closes a page). KEYBOARD.../CONTROLLER... open a read-only page with the current bindings (the
  editing screen is phase 2/3). RESET TO DEFAULTS resets the keyboard and controller maps only.
  Build: `gcc -DKL_BUILD_DLL -shared -o kirby_ui.dll hw/launcher_ui.c hw/config_ini.c hw/window_sdl.c -lSDL2`
  (`hw/tests/run_ui.ps1` does this and also writes `libkirby_ui.dll.a`). `kirby_ui.dll` needs `SDL2.dll`.
  Python/ctypes: `ctypes.CDLL('kirby_ui.dll')`, `CFUNCTYPE(None, POINTER(KlStatus))` for `poll`, `kl_run(ini.encode(),
  poll)`; keep a reference to the callback.
- **Font:** `hw/font5x7.c` (A-Z 0-9 and `. , : - _ / % ( ) ! ? & ' + = < > [ ] * # | ~`; lowercase uses
  uppercase; unknown -> `?`) is unity-included by `window_sdl.c` (so external builds that compile only
  `window_sdl.c` keep working) and exported as `win_font_rows()`.
- **Tests (headless, SDL dummy driver, simulated events):** `hw/tests/test_config_ini.c` (defaults, round trip of
  every field, v1 flat / `[kirby]`, upgrade, parsing, validation, reserved keys, duplicates, garbage struct,
  paths) and `hw/tests/test_kirby_ui.c` linked against the real `kirby_ui.dll` (settings by mouse and keyboard
  saved to the ini, Play disabled in state 1 (pixel check), progress bar pixels, Tab/Shift+Tab, Esc, Select
  ROM, file drop + path, Play with no ROM, Reset, info pages and Back, Quit). Run `hw/tests/run_ui.ps1`;
  frame dump for a visual check: `build/hw/ui_launcher.png`. The config test found a real bug: `set_name`
  copied 31 bytes into `language[8]` (stack overwrite) - fixed.
- **Not done (later phases):** phase 2 keyboard remapping screen with the controller outline (press-a-key
  capture, conflict swap, reserved-key warning); phase 3 controller input and navigating the UI with it;
  `hw_input` (keyboard + controller -> KEYINPUT with the ini bindings; today `win_keys()` still has the old
  fixed Z=A/X=B map). Mouse drag-and-drop of a ROM and real-monitor DPI behavior were tested only with
  synthetic events. Fullscreen/volume are only stored in the ini; applying them is up to kirby.exe.

### 13. Launcher screen adjustments: size/fullscreen/scaling (Gwen)
User feedback with screenshots. All UI text in English.
- **Window size disabled in full screen:** the 1X-6X radios are greyed out, ignore clicks and are skipped by
  Tab/arrows while Fullscreen is checked.
- **SELECT ROM... only when needed:** shown when `state == 0` or `rom_ok == 0`; with a verified ROM the header
  only reads `ROM: <name> OK`.
- **The launcher window follows the options:** logical canvas **480x320**; window = `240*scale x 160*scale`,
  never below 480x320 (so 1X and 2X both give 480x320; 3X = 720x480 ... 6X = 1440x960). At 1X a short caption
  `1X PREVIEW (UI AT 2X)` is drawn. Fullscreen applies to the launcher too (`SDL_WINDOW_FULLSCREEN_DESKTOP`).
  Everything is applied at once on click and saved to the ini; leaving full screen restores the size.
- **Scaling mode (replaces the Integer scaling checkbox):** `Integer | Fit | Stretch`. `kirby.ini`
  `[video] scaling=integer|fit|stretch` (default **fit**). Integer = whole-number zoom (SDL integer scale, black
  bars); Fit = `SDL_RenderSetLogicalSize` with the aspect preserved; Stretch = no letterbox (logical size 0,
  canvas scaled per axis, mouse mapped by window size). Compatibility: with no valid `scaling=`, a file with
  `integer_scale=1` loads as integer and `integer_scale=0` as fit; an explicit valid `scaling=` always wins
  (any key order); an invalid `scaling=` falls back to `integer_scale`, then fit. `cfg_save` writes only
  `scaling=` (no `integer_scale`). `KirbyConfig.integer_scale` was replaced by `int scaling`
  (`CFG_SCALING_INTEGER/FIT/STRETCH`), `cfg_scaling_name()`.
- **Shared by kirby.exe:** `hw/window_sdl.c` now has `win_set_scaling(mode)` / `win_get_scaling()` (mode = the
  `CFG_SCALING_*` value; can be called before or after `win_open`) so the game window uses the same three
  behaviours. rt_input.c already includes `config_ini.c` and reads `c.scaling`.
- **Bug found while testing:** in SDL2 the logical size lives in the renderer scale, so calling
  `SDL_RenderSetScale(1,1)` every frame cancelled it (clicks missed once the window was not 480x320). Only the
  stretch mode touches the render scale now.
- **Tests:** `test_config_ini.c` (scaling round trip, case-insensitivity, absent/invalid, every v1/v2
  `integer_scale` combination, out of range, re-save) and `test_kirby_ui.c` against the real `kirby_ui.dll`
  (window size per scale incl. the 480x320 floor, caption at 1X, fullscreen disables the sizes and focus skips
  them, Integer/Fit/Stretch saved, applies immediately, SELECT ROM visibility matrix, stretch click mapping).
  `hw/tests/run_ui.ps1`; frame at native size: `build/hw/ui_launcher.png`.
- **Not done:** the full-screen path was only exercised with the SDL dummy video driver (flag and size
  logic); real monitor/DPI behaviour and the integer mode on a non-multiple window are untested by eye.

### 14. Launcher: editable KEYBOARD / CONTROLLER pages, stick settings, live indicator (Gwen) - phases 2 and 3
All UI text in English. Files: `hw/launcher_ui.c` (kirby_ui.dll; the file list is unchanged), `hw/config_ini.c`.
- **GBA outline (own vector art, no Nintendo asset):** rounded body, L/R shoulders on top, d-pad cross on the
  left, A and B circles on the right (B left of and a little below A), SELECT/START pills at the bottom, a dark
  screen in the middle. Every button is clickable and shows its current key/button (inside the shoulders and
  pills, next to the d-pad arms, under A/B). Two tabs (KEYBOARD / CONTROLLER) plus RESET (this tab only) and
  BACK sit on top; the main page buttons `KEYBOARD...` / `CONTROLLER...` open the matching tab.
- **Remapping:** click, or Enter/Space/pad south on the focused button -> `PRESS A KEY FOR A...` (controller:
  `PRESS A BUTTON FOR A...`), the next key/button becomes the binding and is written to `[keyboard]` /
  `[controller]` immediately. Esc cancels (does not leave the page; a click elsewhere also cancels). A name
  already used by another GBA button is **swapped** (`A = Q (SWAPPED WITH B)`). **Esc, F11 and F1 are refused
  with a warning and the capture continues**; names the ini cannot hold (`= ; # [ ]`, empty) are refused too.
  Keyboard names are `SDL_GetKeyName`, pad names `SDL_GameControllerGetStringForButton`. Without a connected
  controller the capture is refused (`NO CONTROLLER CONNECTED`). While waiting for a pad button, keyboard keys
  are ignored (except Esc) and vice versa.
- **Defaults by position** (config_ini.c, unchanged): keyboard X=A, Z=B, A=L, S=R, Return=Start,
  Backspace=Select, arrows; controller A = SDL `b` (east), B = SDL `a` (south), shoulders, `start`, `back`, d-pad.
- **Controller tab extras:** checkbox `LEFT STICK AS D-PAD` (`[controller] stick_as_dpad`), slider `DEADZONE`
  0..32767 (default 8000, saved as `deadzone`; Left/Right on the focused slider = +-1000), and a **live
  indicator**: circle with the dead-zone radius + a dot at the current left-stick position read with
  `SDL_GameControllerGetAxis` (green dot when outside the zone). Only the first connected controller is used;
  `NO CONTROLLER` is drawn when none. Hot-plug (`SDL_CONTROLLERDEVICEADDED/REMOVED`) re-opens the first one.
- **Navigating the whole UI with the controller:** d-pad = Up/Down/Left/Right, **south = OK, east = back** (a
  no-op on the main page: it never quits), left stick edge-triggered as a d-pad. Keyboard Tab/arrows as before;
  on the editor pages the arrows use spatial navigation (nearest widget in that direction).
- **Mouse bug fixed (found with the new window-id events):** with `SDL_RenderSetLogicalSize` active, SDL
  converts the coordinates of real mouse events (those that carry the window id) in place; the UI then
  converted them again, so real clicks missed in any window that was not exactly 480x320 (the earlier tests
  only passed because they pushed events without a window id). The launcher no longer uses SDL's logical size:
  the UI is drawn into a 480x320 target texture and copied to the window with its own integer/fit/stretch
  rectangle, and `to_logical()` is the single, deterministic window->canvas mapping. Events whose window id is
  not the launcher's are ignored.
- **BOM (Poppy's QA finding 5):** `cfg_load` skips a UTF-8 BOM; tests added.
- **Tests (`hw/tests/run_ui.ps1`, SDL dummy driver, every event carries `kl_test_window_id()`):**
  `test_kirby_ui_edit.c` - keyboard remap, swap, F11/F1 refusal, Esc cancel, Enter to capture, RESET,
  spatial navigation, tabs, foreign window id ignored; controller capture (simulated `SDL_CONTROLLERBUTTONDOWN`),
  swap, keyboard ignored while waiting, stick checkbox, deadzone slider, live indicator pixels (outside /
  inside the zone, no-controller text), RESET, d-pad/south/east navigation, left-stick navigation, south on
  QUIT. `test_kirby_ui.c` / `test_config_ini.c` still pass; frames: `build/hw/ui_keyboard.png`, `ui_controller.png`.
  Test hooks added: `kl_test_window_id`, `kl_test_capture`, `kl_test_note`, `kl_test_fake_pad`, `kl_test_pad_present`.
- **Not done:** a real gamepad was not exercised (events were simulated; the dev machine may have one, the tests
  force `fake_pad`); game (kirby.exe) input already reads the same ini via rt_input.c; capturing joystick
  hats/axes (only SDL game-controller buttons) and non-SDL-mapped controllers.

### 15. Launcher: SAVE section (OPEN FOLDER / EXPORT... / IMPORT...) (Gwen)
Contract: NOTES 'Gerenciamento de save: plano e contrato (Riven)'. English UI; same 3 source files for kirby_ui.dll.
- **New `kl_run` return codes** (enum in `launcher_ui.h`, documented there): `KL_OPEN_SAVE_DIR = 3`,
  `KL_EXPORT_SAVE = 4`, `KL_IMPORT_SAVE = 5`. The launcher performs the action and calls `kl_run` again (the ini
  and settings are re-read each call). `KL_QUIT = 0`, `KL_PLAY = 1`, `KL_PICK_ROM = 2` are unchanged.
- **UI:** a `SAVE` row with `OPEN FOLDER`, `EXPORT...`, `IMPORT...` between AUDIO and INPUT. The three buttons are
  **disabled (greyed, no click/Enter) unless a verified ROM is loaded** (`rom[0] != 0 && rom_ok == 1`; no ROM, or an
  unverified ROM, disables them). OPEN FOLDER and EXPORT... return their code immediately.
- **IMPORT confirmation lives in the UI:** IMPORT... opens a modal dialog over the dimmed screen,
  `OVERWRITE CURRENT SAVE?` / `A BACKUP IS KEPT` with `[YES] [NO]`. Focus starts on **NO** (the safe answer); NO,
  Esc, or the controller's east button close it without leaving the screen; Left/Right/d-pad switch between the two;
  only YES returns `KL_IMPORT_SAVE`. While the dialog is open nothing behind it is clickable or focusable. By the
  contract, the launcher must back up the current `.sav` before overwriting and validate 32768 bytes - the UI has
  already asked the user, so do not ask again.
- **Keyboard / controller:** the three buttons are in the normal focus order (MUTE -> OPEN FOLDER -> EXPORT... ->
  IMPORT... -> KEYBOARD...; Left/Right move along the row; d-pad and south/east work the same as on the keyboard).
- **Layout re-flowed** to make room without anything stacking: status panel 58..100, rows every 26 px (window size
  y=106, fullscreen 132, scaling 158, volume 184, mute 210, SAVE 236, INPUT 262, PLAY/QUIT 290), footer 313. Texts
  that could collide were bounded: the ROM line is at most 40 characters (long names become `...<last 19>`), the
  status free text is clipped to 170 px so it never runs under the progress bar (now at y=87).
- **Layout tests use real geometry, not eyeballing:** the UI records every free text and the progress bar of the
  last frame (`kl_test_text_count/kl_test_text/kl_test_bar`); `test_kirby_ui_save.c` checks, for 8 states (no ROM,
  preparing, building with a long text, ready, unverified with a very long path, error, 1X caption, full-screen
  stretch), that no widget leaves the canvas, no two widgets overlap, no text is behind a widget or the progress
  bar, no two texts overlap, and the slider's `100%` stays clear. It also compares snapshot pixels (enabled vs
  disabled button) and dumps frames: `build/hw/ui_save.png`, `ui_save_confirm.png`, `ui_save_long.png`,
  `ui_save_building.png`.
- **Tests (`hw/tests/run_ui.ps1`, every event carries the window id):** enabled/disabled by ROM state; codes 3, 4, 5
  by mouse, keyboard (Enter/Space/Esc/Left) and simulated controller; the dialog is modal; NO then IMPORT again;
  navigation through the SAVE row. All earlier suites (config, UI, editor) still pass.
- **Not done:** the real actions (open the Explorer, file dialogs, backup/validation, the `.sav` rotation) belong to
  the Python launcher (Riven) and the runtime flush (Nasus); the UI only reports the intent.

### 16. Cable link P1, hw/ side: SIO multi-play model, link harness, ONLINE UI draft (Gwen)
Plan: NOTES 'Online / cabo link: plano'. Nasus owns the SIO use in the runtime; this is the hardware model + the
test bench. English UI text in the draft below.

**A. SIO multi-play in `hw/hw.c` (per unit; unchanged outside multi-play)**
- Multi-play = `SIOCNT` bits 12-13 == 2 and `RCNT` bit 15 == 0 (the game's `0x2003`/`0x6003` writes). Outside it SIOCNT /
  SIOMULTI keep the old plain-storage behaviour (checked by the tests).
- `SIOCNT` in multi-play: bits 0-1 baud, 12-14 stored; **bit 2 SI** (1 = child), **bit 3 SD** (cable: >= 2 plugged units
  in multi-play), **bits 4-5 ID** (0 master, 1-3 child; set when a transfer ends, 0 before the first one), **bit 6 error**,
  **bit 7 busy / start**. Bits 2-6 are not writable. Only the master (slot 0) starts a transfer (writing bit 7); a start
  while busy, or from a child, is ignored. `SIOMULTI0-3` are read-only in multi-play and read 0xFFFF until a transfer ends.
- Start: busy set, SIOMULTI0-3 := 0xFFFF, the unit's `SIOMLT_SEND` is handed to the cable hook. End (`hw_sio_deliver`):
  the four values are stored, ID/error set, busy cleared, **serial IRQ (IF bit 7)** raised if SIOCNT bit 14 is set.
- API (hw.h, same on the real runtime instance): `hw_sio_set_link(ctx, start_cb)`, `hw_sio_set_slot(0..3)`,
  `hw_sio_set_sd`, `hw_sio_multi_mode`, `hw_sio_send_value`, `hw_sio_busy`, `hw_sio_slot_cycles` (18 bit times at the
  current baud: 2610 cycles at 115200, 31446 at 9600), `hw_sio_begin`, `hw_sio_deliver`.
- **No cable attached** (`start_cb == NULL`, i.e. today's kirby.exe): hw finishes a master transfer by itself after one slot
  time with SIOMULTI0 = own data, 1-3 = 0xFFFF, **error bit set**, IRQ raised, so the game cannot hang on a busy bit that
  never clears. Smoke test: kirby.exe 2500 frames, 0 unresolved, no stall.
- **Modelling assumptions to confirm in P0** (GBATEK is vague here): error = master transmitting with nobody connected, a
  unit leaving before its turn, or the master leaving; SD = at least two plugged units in multi-play; ID is valid only
  after the first transfer; a transfer lasts (highest present slot + 1) slot times; each unit's value is sampled at its own turn.

**B. Several instances without the game: `HW_PREFIX`** - compiling `hw.c` with `-DHW_PREFIX=i0_` renames every public
symbol (including the `gba_read/gba_write` callbacks) so 2-4 copies link into one program with no shared state.

**C. `hw/link_harness.[ch]` (the cable model for tests; the future network layer replaces it).** `lh_init(n)`,
`lh_connect(slot, plugged)` (also mid-transfer), `lh_step(cycles)`, `lh_read/lh_write(slot, ...)`, helpers
`lh_enter_multi`, `lh_set_send`, `lh_master_start`, `lh_detach`, and observers (`lh_last_order`, `lh_last_value`,
`lh_last_error`, `lh_transfers`). Slot k puts its data on the wire at `start + k*slot_cycles`; the end is at
`(max slot + 1) * slot_cycles`; participants are plugged units in multi-play at the start.

**D. Tests: `hw/tests/test_link.c`, run `hw/tests/run_link.ps1` (no SDL; OK).** Covers: whole life of a 2-unit transfer
(busy/SIOMULTI at start, exact end, IRQ, IF clear, ID/SI/SD bits); wire order master -> 1 -> 2 -> 3 and sampling at each
unit's turn; 4 units with an absent slot (0xFFFF, order 0,1,3, ID 3 kept, no error, length 4 slots); 3 units; master alone
(error, IRQ) and a console with no link attached; **child dropping mid-transfer** (aborted at once, 0xFFFF + error for the
others, later children still answer; leaving after its turn is clean); **master dropping mid-transfer** (children aborted,
no late delivery); start while busy; child cannot start; register rules; baud 9600 timing; IRQ off; unit not in
multi-play; SD vs plugging; instance isolation.

**E. ONLINE section: UI PLAN ONLY (not implemented).** The main page has no free row, so ONLINE is a page like KEYBOARD:
a button `ONLINE...` appears in the header slot of `SELECT ROM...` (298,34,170,22) whenever a verified ROM is loaded
(that slot is empty then); focus order after SELECT ROM. Page layout (480x320 canvas, 5x7 font, same widgets):
```
 ONLINE  (CABLE LINK)                                   [BACK]
 MODE   [HOST] [JOIN]            ADDRESS [ 192.168.1.20      ]   <- JOIN only
 PORT   [ 7777 ]                 YOUR ADDRESS 192.168.1.5:7777   <- HOST only, [COPY]
 PLAYERS
  P1 HOST   KIRBY_A     READY      0 MS
  P2        FRIEND      CONNECTED  23 MS
  P3        -           EMPTY
  P4        -           EMPTY
 STATE: Waiting for players...                 [CONNECT] / [START HOST]   [LEAVE]
```
Strings (UI text, uppercase font): `ONLINE`, `HOST`, `JOIN`, `ADDRESS`, `PORT`, `PLAYERS`, `P1 HOST`..`P4`, `EMPTY`,
`CONNECTING...`, `CONNECTED`, `READY`, `LOST`, `PING: 23 MS`, `STATE: Not connected | Waiting for players... |
Connecting... | Connected (2 players) | Desync detected | Connection lost`, buttons `START HOST`, `CONNECT`, `LEAVE`,
`COPY`, `START GAME` (host, enabled with >= 2 connected). Slots 1-4 follow the cable: the host is always P1, joiners
take P2-P4 in arrival order (ID = slot). Ping colours: green < 60 ms, yellow < 120 ms, red above (the plan says the
game needs < ~60 ms). Fields: ADDRESS and PORT are text boxes (`SDL_StartTextInput`, digits/dots/letters/colon only,
max 45 chars; PORT 1-65535, default 7777, saved in `kirby.ini [online] host=`, `port=`, `name=`); Enter/click focuses
a field, Esc leaves it; controller: south on a field opens an on-screen digit pad (later). Data from the launcher: a
second poll `KlOnlinePoll(KlOnline *out)` with `{ int state; int slots[4] (0 empty 1 connecting 2 connected 3 ready
4 lost); int ping_ms[4]; char names[4][16]; char my_address[48]; char message[96]; }`; new return codes (proposal)
`KL_ONLINE_HOST = 6`, `KL_ONLINE_JOIN = 7`, `KL_ONLINE_LEAVE = 8`, `KL_ONLINE_START = 9`, each followed by `kl_run`
again, like the SAVE codes. The layout must pass the same geometry checks as the SAVE section (no overlapping
widgets/texts). Implementation waits for P2 (transport) - nothing in `launcher_ui.c` changes yet.

### 17. Link study section 9 checked against hw/ (Gwen)
Poppy's 'Minimo que o hw/ precisa emular' items marked [V]. Result of each (tests: `hw/tests/test_link.c` T16-T19,
`hw/tests/run_link.ps1`; the old hw suite, the UI tests and a 2500-frame kirby.exe run still pass):
- **RCNT (0x04000134):** was NOT reset to 0x8000 - **fixed** (`hw_reset` writes 0x8000). Multi-play requires RCNT bit 15 = 0,
  so until the game writes RCNT = 0 a SIOCNT `0x2003` + start bit does nothing (tested), exactly the "not a MultiBoot client"
  situation. RCNT stays a plain 16-bit register.
- **SIOCNT / SIOMLT_SEND at 8/16/32 bits:** 16 bits and byte writes already worked; verified now: `ldr [SIOCNT]` =
  SIOCNT | SIOMLT_SEND << 16, byte read/write of SIOCNT and SIOCNT+1 (a `strb` of `old | 0x80` on the low byte starts the
  transfer; clearing bit 6 of the HIGH byte clears the IRQ-enable bit), SIOMLT_SEND as two bytes or one halfword.
  **One fix:** a 32-bit write at SIOCNT now lands SIOMLT_SEND (high half) BEFORE SIOCNT, so a start bit in the same word
  transmits the NEW send value (before, it would have sent the old one). `str 0x2000` sets mode multi / baud 0 and clears SEND.
- **SIOMULTI as 32-bit pairs:** `ldm` style reads of 0x04000120 / 0x04000124 return MULTI0|MULTI1<<16 and MULTI2|MULTI3<<16;
  byte reads work; writes (8/16/32) are ignored in multi-play (read-only), plain storage outside it (unchanged).
- **Timer3 (0x0400010C/E):** already correct, now proven: a 32-bit write `0x00C0BBBC` = TM3CNT_L 0xBBBC reload + TM3CNT_H
  enable|IRQ|F/1; the counter loads 0xBBBC on the 0 -> 1 edge and counts 1 per cycle; overflow after **exactly 17476 cycles**
  raises **IF bit 6**, reloads 0xBBBC, period 17476; IE bit 6 gates the CPU interrupt. **Restart semantics:** rewriting
  TM3CNT_H while it runs does NOT restart (real hardware: only a 0 -> 1 edge reloads); stop (write 0) then the 32-bit write
  restarts it. The game's code does the stop-then-start sequence (`str 0` to 0x0400010C then `strh 0xC0` / the 32-bit word),
  so this is the behaviour it needs; if the runtime ever relies on a restart WITHOUT a stop, say so and it can be changed.
- **Timing (T19):** 4 slots x 18 bit times at 115200 baud = **10440 cycles** (`hw_sio_slot_cycles` = 2610 per unit) < 17476.
  Test: six exchanges driven by the master's Timer3: each finishes 7036 cycles before the overflow, the IF-bit-6 handler's
  `ldm` of SIOMULTI0-3 sees the values of the exchange that JUST finished (not the previous one), the transfer counter
  matches. Negative control: at 9600 or 57600 baud 4 slots take longer than 17476 cycles, the exchange is still busy at the
  overflow and the handler would read 0xFFFF / the previous exchange - so the game needs 115200 baud (the 0x2003 literal).
- **Not changed:** the ONLINE launcher screen draft (item 16E) - nothing in this check affected it. Nothing implemented in the UI.

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

### kirby.ini v2, F1, entrada unificada e mensagens em ingles (Nasus)
- **`kirby.ini`** (`rt_input.c`, parser proprio, sem mudar a linha de comando): caminho = `--ini F` ou
  `kirby.ini` ao lado do save (ou da ROM se `--sav none`). Secoes v2 `[video] scale fullscreen
  integer_scale vsync`, `[audio] volume(0-100) mute`, `[keyboard] a b l r start select up down left
  right` (nomes SDL), `[controller]` (nomes de `SDL_GameControllerGetStringForButton`) + `stick_as_dpad`,
  `deadzone`; tambem le o v1 (chaves soltas/`[kirby]`). Valor invalido -> padrao; comentarios `;`/`#`.
  Padroes: teclado A=X B=Z L=A R=S Start=Return Select=Backspace + setas; controle A=`b` (leste) B=`a`
  (sul), L/R=ombros, start/back, dpad. `--window N` na linha de comando tem prioridade sobre `scale`.
  Aplicado: escala, tela cheia inicial, escala inteira (`SDL_RenderSetIntegerScale`), vsync
  (`win_open_ex`), volume (escala as amostras antes do `audio_push`), mudo (nao abre o audio).
  Autoteste: `rt_boot.exe baserom.gba --selftest-ini` (v2 completo, invalidos, v1, vazio): OK.
- **F1** sai do processo com **codigo 77** (grava o `.sav` no `atexit`); **Esc**/fechar = 0; **F11**
  tela cheia. Reservadas: se o ini mapear Esc/F1/F11 a um botao, a ligacao e ignorada. Testado com
  `KIRBY_TEST_HOTKEY="F1@90"` (gancho de teste): exit 77 e `.sav` de 32 KiB gravado.
- **Entrada unificada** (`rt_input_poll`, no `kirby.exe`; substitui `win_keys()` do `window_sdl.c`):
  teclado por scancode + `SDL_GameController` (ate 4), **hot-plug** por varredura a cada ~30 quadros
  (os eventos do SDL sao consumidos por `win_poll`), **zona morta** configuravel, **analogico
  esquerdo como direcional** opcional, mapeamento por **posicao** (GBA A = leste, B = sul), pares de
  direcoes opostas se cancelam. **Nao testado com hardware** (sem controle aqui): so compila/linka e o
  caminho do teclado roda; peco teste manual com um controle.
- **Mensagens em ingles** (kirby.exe/rt_boot): MessageBox "ROM not found. Drag the .gba file onto
  kirby.exe, or put baserom.gba in the same folder.", avisos de audio/janela, rotulos
  `HALT`/`WARNING`, resumo final. **Tokens de log que mudaram** (consumidores: `program.py`, tests,
  `tools/fuzz_run.py` do Riven):
  | antes | agora |
  |---|---|
  | `PARADA:` / `AVISO:` | `HALT:` / `WARNING:` |
  | `PARADA: endereco sem funcao recompilada ...` | `HALT: address with no recompiled function (RAM overlay or ROM exit)` |
  | `PARADA: funcao saiu sem avancar o pc ...` | `HALT: function left pc unchanged (target outside the compiled set)` |
  | `nao_resolvidos=N` | `unresolved=N` |
  | `IRQ entregues=` (`pelo handler`/`pelo runtime`/`IF limpo pelo runtime`) | `IRQs delivered=` (`by handler`/`by runtime`/`IF cleared by runtime`) |
  | `VELOCIDADE:` / `TRECHO q.` / `quadros atrasados` | `SPEED:` / `SEGMENT f.` / `late frames` |
  | `INTERPRETADOR: N entradas, M instrucoes` | `INTERPRETER: N entries, M instructions` |
  | `RT_STOP frames/...` | igual (`first VBlankIntrWait` no lugar de `primeira VBlankIntrWait`) |
  Formato de `unresolved.log` e `[overlay]` do Python nao mudou.

### Modos de escala (`[video] scaling=integer|fit|stretch`) e parser unico (Nasus)
- **`scaling`** (padrao **fit**): `integer` = multiplos inteiros (borda nos dois eixos quando nao cabe
  exato); `fit` = escala fracionaria preservando 3:2 (borda so nas laterais num 16:9); `stretch` =
  preenche a janela (distorce). Legado: `integer_scale=1` -> integer, `0` -> fit; `scaling=` vence
  (regra do `hw/config_ini.c`). Implementado em `rt_window.c::rt_apply_scaling`: `window_sdl.c`
  usa `SDL_RenderSetLogicalSize(240,160)` (= fit); integer liga `SDL_RenderSetIntegerScale`;
  stretch remove o tamanho logico (`SetLogicalSize(0,0)`, `RenderCopy(..., NULL, NULL)`). Aplica na
  abertura (e em tela cheia, F11). Linha de comando: `--scaling integer|fit|stretch` (prioridade
  sobre o ini), sem mudar o resto.
- **Parser unico**: `rt_input.c` inclui `hw/config_ini.c` (uma unidade de traducao; sem SDL) e
  usa `cfg_load` (sempre validado: `scale=2x` -> padrao, deadzone ate 32767, tecla reservada e
  duplicadas resetam o teclado, BOM etc. = o que a UI faz). O parser proprio foi removido; o
  `rt_cfg`/`rt_cfg_load` seguem como API fina sobre `KirbyConfig` (harness da Poppy continua
  compilando so `rt_input.c`).
- **Testes**: `tests/test_launcher_qa.py` - os 4 `expectedFailure` do jogo (`scale=2x`, deadzone
  maximo, reservada/duplicada, parser compartilhado) agora PASSAM e os decoradores foram removidos;
  `ParserParityTest`/`SharedIniFileTest`/`ScalingParseParityTest` verdes. Ajuste necessario no
  teste: `FIELD_SECTION` troca `integer_scale` por `scaling` (o `cfg_save` da Gwen nao grava mais
  `integer_scale`) e o harness do jogo imprime `scaling=`.
  **Ainda vermelhos (nao sao do jogo; expectativas antigas)**: `IniV2ContractTest` (`V2_SPEC` ainda
  tem `integer_scale` com padrao 1) e `ScalingParseUITest` (espera padrao `integer`; o config_ini.c
  novo e `fit`; ver o pedido do usuario) - 35 falhas ao todo, todas dessas duas classes + parity
  de `launcher.show` ja corrigidas por esta mudanca.
- **Retangulos de destino** (`kirby.exe --selftest-video`: janela dummy do SDL, modo aplicado,
  retangulo medido por `SDL_RenderLogicalToWindow`; todos batem com `RECT_TABLE` da Poppy):
  | janela | integer (x,y,w,h) | fit | stretch |
  |---|---|---|---|
  | 1920x1080 | 240,60 1440x960 (6x) | 150,0 1620x1080 (6,75x) | 0,0 1920x1080 |
  | 1366x768 | 203,64 960x640 (4x) | 107,0 1152x768 (4,8x) | 0,0 1366x768 |
  Em tela cheia 1080p o `fit` so deixa as laterais (150 px de cada lado); `integer` deixa 60 px
  em cima/embaixo e 240 px nas laterais. Nao testei em monitor real.

### Controle: um controle, analogico com histerese, hot-plug, recarga do ini (Nasus)
- **Config**: `[keyboard]`/`[controller]` lidos por `cfg_load` (`hw/config_ini.c`, mesmo parser da UI).
  **Recarga ao voltar do launcher:** F1 -> exit 77 -> o launcher reabre o `kirby.exe` (processo novo
  => `cfg_load` de novo). Verificado: duas execucoes seguidas com ini diferente mostram em
  `kirby.log` `settings loaded: ... A=Q ... scaling=stretch` e depois `A=E ... deadzone=12000 ...`.
- **Um controle so**: o primeiro `SDL_GameController`; se desconectar, passa ao seguinte (failover);
  um segundo controle conectado depois NAO rouba a entrada. Hot-plug: varredura a cada ~30 quadros
  e deteccao de remocao a cada quadro (os eventos do SDL sao consumidos por `win_poll`).
- **Analogico esquerdo como direcional** (`stick_as_dpad`, padrao 1): so o ESQUERDO (o direito nunca
  vale); 8 direcoes por combinacao dos eixos; **histerese**: entra em 100% da zona morta, sai em 85%
  (zona morta 8000: entra em 8000, solta abaixo de 6800); dpad fisico continua valendo; botoes por
  posicao (GBA A = leste, B = sul). Teclado OU controle (combinam); Esc/F1/F11 reservadas.
- **Log** (`kirby.log` ao lado do ini, tambem no stderr): `settings loaded: ...`, `controller
  connected: <nome> (using it; others are ignored)`, `controller disconnected: <nome>`.
- **Teste** `kirby.exe --selftest-input` (SDL dummy + joystick VIRTUAL do SDL 2.32,
  `SDL_JoystickAttachVirtual/SetVirtualAxis/SetVirtualButton`, sem hardware): leste->A, sul->B,
  L+Start, dpad, zona morta 7900 nada / 8100 direita / 7000 mantem / 6700 solta / 7500 nao reentra,
  diagonais cima-esq e baixo-dir, analogico direito ignorado, `stick_as_dpad=0`, segundo controle
  ignorado, desconectar o primeiro passa ao segundo, todos fora = parado sem travar, reconexao:
  **OK**. Observacao: esta maquina tem um "Xbox 360 Controller" real; o teste ignora controles
  reais (`SDL_JoystickIsVirtual`). Harness da Poppy (ParserParity/SharedIni/ScalingParity): verde.
- Limite: `SDL_PushEvent` de CONTROLLERBUTTON nao muda o estado lido por `SDL_GameControllerGetButton`;
  por isso o teste usa joysticks virtuais (caminho real de SDL, nao simulacao de eventos).

### Save atomico da SRAM (Nasus) - `recomp/runtime/rt_save.c`
- **Suja**: o barramento incrementa `gba_sram_gen` a cada escrita que MUDA a SRAM (0x0E000000..
  0x0E007FFF; escrever o mesmo valor nao suja - o verify do jogo nao gera flush).
- **Debounce**: `rt_save_tick()` (1x por quadro, em `VBlankIntrWait`; caminho comum = 1 comparacao)
  grava **~1 s apos a ULTIMA escrita**, ou no maximo **5 s apos a primeira** se o jogo escreve sem
  parar (so assim nunca fica sem gravar).
- **Atomico**: escreve `<save>.tmp` (`fwrite` + `fflush` + `_commit`/FlushFileBuffers) e
  `MoveFileExW(tmp, sav, REPLACE_EXISTING|WRITE_THROUGH)` (caminhos UTF-8 -> `_wfopen`/wide, com
  acento e espaco; fora do Windows `rename`). O `.sav` **nunca e escrito diretamente**.
- **Tambem grava** no `atexit` (cobre F1 = 77, Esc e janela fechada), em F1 (explicito), em
  `CTRL_CLOSE/C/BREAK/LOGOFF/SHUTDOWN` (`SetConsoleCtrlHandler`, outra thread, com secao critica) e
  `SIGTERM`. `kirby.exe` e app grafico: o evento de console so chega no `rt_boot`; no jogo vale o
  fechar da janela (`SDL_QUIT` -> atexit). Nao testei `taskkill` sem `/F` nem logoff.
- **Falha**: 1 aviso por sequencia no `kirby.log` (`[save] could not write <arq> (error N, <motivo>);
  will retry`), nova tentativa no proximo ciclo (1 s), aviso de recuperacao; o jogo nao trava.
- **Custo medido**: (a) simulado, jogo escrevendo 64 B/quadro por 1 min (3600 quadros, relogio
  falso): **12 flushes/min**, custo medio **8,3 us por quadro** (30 ms no minuto inteiro, ~2,6 ms por
  gravacao); (b) jogo real (8000 quadros = 2,2 min de jogo, menu -> 1a fase): **2 flushes**
  (formatacao do save), 13,4 ms de escrita no total (~6,7 ms cada, com fsync), tick 1,7 us/quadro;
  reabrindo com o save existente: **0 flushes**. Um flush de ~3-7 ms cai num quadro de 16,7 ms: sem
  impacto perceptivel com a folga de 9x. `rt_boot --bench`/`--window` imprimem `SAVE: ...`.
- **Testes** (`tests/test_save.py`, usa `rt_boot.exe`; `KIRBY_RT_BOOT` para apontar outro):
  `--selftest-save DIR` (relogio falso: sem flush no mesmo quadro nem a 0,5 s; 1 flush a ~1 s;
  `.sav` com o byte escrito e 32768 bytes; sem `.tmp` sobrando; escrita de mesmo valor nao suja;
  escrita continua = 10-13 flushes/min; falha em diretorio inexistente -> tentativas repetidas e
  recuperacao): OK. **Kill**: um processo grava a SRAM inteira sem parar (cada geracao = 1 byte
  uniforme) e o teste o mata com `TerminateProcess` (= kill -9) em 40 instantes aleatorios:
  **40/40 `.sav` com 32768 bytes uniformes** (antigo ou novo, nunca parcial).

### Cabo link P1: SIO multi-play + transporte plugavel (Nasus) - `rt_sio.c`, `rt_sio.h`, `rt_sio_test.c`
**Semantica geral do GBATEK** (a Poppy ainda nao entregou 'Protocolo de link do KatAM'; ajustar depois).
- **Registradores** (interceptados no barramento do runtime, `gba_bus.c::io_read/io_write`, so se o link
  esta ligado; `mode=off` = `rt_sio_cur == NULL` = passa direto ao hw/, **comportamento identico ao de
  hoje**): SIOMULTI0-3 `0x04000120-26` (somente leitura), SIOCNT `0x04000128`, SIOMLT_SEND `0x0400012A`,
  RCNT `0x04000134`, mais o **bit 7 (serial) de IF `0x04000202`** (o hw/ nao tem o IRQ serial; leitura de
  IF mescla o bit, escrita w1c o limpa). Acessos de 8/16/32 bits.
- **SIOCNT**: gravaveis baud (bits 0-1), modo (12-13: 2 = multi-play) e IRQ (14); lidos: bit 2 SI (0 =
  mestre, 1 = filho), bit 3 SD (1 = todos os `players` presentes), bits 4-5 ID (valem apos a 1a troca),
  bit 6 erro (sempre 0), bit 7 Start/ocupado. So o MESTRE inicia (filho: Start ignorado); so em multi-play
  (`SIOCNT` modo 2 e `RCNT` bit 15 = 0).
- **Troca**: Start do mestre -> SIOMULTI0-3 = FFFF, ocupado ate o fim da duracao (72 bits = 4 x 18 bits ao
  baud: 115200 -> 10 486 ciclos), depois SIOMULTIx = valores, ocupado = 0, ID = numero do jogador, IRQ serial
  se o bit 14 esta ligado. Jogador ausente = 0xFFFF. **Sem transporte (cabo desconectado)**: mestre sozinho,
  SIOMULTI0 = o proprio valor, resto FFFF, SD = 0. Os filhos ficam ocupados quando a mensagem do mestre
  chega e terminam apos a mesma duracao.
- **Tempo/IRQ**: o SIO avanca no mesmo relogio do hw (`run_cycles` limita o passo ao fim da troca) e o IRQ
  serial entra por `rt_irq_service` (`hw_irq_pending() | rt_sio_irq_pending()`; exige IME e IE bit 7), o
  mesmo caminho do VBlank (contexto salvo, vetor `[0x03007FFC]`, congelamento do relogio no handler).
- **Interface `LinkTransport`** (`rt_sio.h`): `publish(meu SIOMLT_SEND)`, `exchange(my) -> 4 valores`
  (so mestre), `poll(evento ROUND)`, `connected_mask`, `close`. Implementacoes: **null** (jogador
  sozinho), **hub** (N instancias no MESMO processo, para testes), **local** (TCP 127.0.0.1: o host
  escuta, os filhos conectam e recebem id 2,3,4 na ordem; mensagens de 12 bytes HELLO/SEND/ROUND; sockets
  nao bloqueantes; filho solto = slot FFFF; reconexao de filho nao implementada).
  Modelo: cada filho publica o SEND quando o jogo o escreve; no Start o mestre usa o ultimo valor
  publicado (nao e lockstep; ver P2).
- **Configuracao proposta (kirby.ini NAO foi alterado)**: nova secao
  ```
  [link]  mode=off|host|join   players=2..4   port=47900   host=127.0.0.1
  ```
  `mode=solo` (extra) usa o transporte null para teste. Lida por `rt_sio_read_ini` (o parser compartilhado
  ignora secoes desconhecidas); a linha de comando tem prioridade: `--link off|host|join|solo`,
  `--link-players N`, `--link-port P`, `--link-host H`. **Gwen:** quando quiser, adicione `[link]` ao
  `config_ini.c`/launcher (mesmos nomes) e eu passo a usar o parser compartilhado.
- **Testes**: `rt_boot.exe ROM --selftest-link` (sem hardware de rede): null (inicio/duracao/IRQ so se
  habilitado/modo nao-multi/RCNT geral/32 bits e bytes), hub 2, 3 e 4 jogadores (2 trocas cada, SI/SD/ID,
  IRQ e w1c do IF, filho nao inicia, jogador solto vira FFFF e SD cai) e TCP real em 127.0.0.1 com 3
  instancias no processo (ids 2 e 3 na ordem, 2 trocas, filho sai -> slot FFFF e o outro filho segue):
  **OK**. Regressao `mode=off`: 3000 quadros, quadro final md5 identico ao de antes (`0455812b...`), tambem
  com `--link solo`.
- **Dois processos de verdade** (`--link host` + `--link join --link-port P`): conectam ("connected as
  player 2 of 2"); no menu inicial **0 trocas** (o jogo nao usa o link ali). Achado para a Poppy: depois de
  conectar, o processo FILHO fica preso num laco (pc 0x08151DB0, interpretado) sem voltar a
  `VBlankIntrWait` - o menu inicial do KatAM reage a SD = 1 / SI = 1 esperando algo do mestre; precisa do
  estudo do protocolo (P0) para saber o que responder.
- Limites: lockstep/hash por quadro e atraso de entrada (P2), internet (P3), UI (P4) nao feitos; o modo UART
  e normal 8/32 bits nao sao emulados; erro (bit 6) nunca e gerado; ordem de transmissao = indice.

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

### Cabo link P1.5: duas instancias reais (Nasus) - resultado parcial
> **Superado pelo 'Cabo link P2a' (lockstep por quadro):** os bloqueios 1-3 abaixo foram resolvidos (partida alcancada, fluxo deterministico, join fecha limpo).
- **0x08151DB0 nao era travamento**: e a espera de DISPSTAT do titulo (laco de VBlank por polling). Em modo link o jogo nao chama VBlankIntrWait nessa tela, entao o contador de quadros parava e o `--frames`/hooks nao andavam. Correcao: `grab_frame(Cpu*)` em `gba_bios.c` conta quadros pelo hw quando passam >3 quadros sem VBlankIntrWait. **So com `rt_sio_cur != NULL`**: mode=off conta so por VBlankIntrWait (um teste do boot, IRQs=600/601, pegou a diferenca e foi corrigido).
- RCNT reseta em 0x8000 no runtime (SIO desligado ate o jogo escrever RCNT=0). Testes de `--selftest-link` escrevem RCNT=0.
- Condicoes do mestre conferidas no `rt_sio.c` (SD=1, SI=0, ID=0, start so do mestre, Timer3 reload 0xBBBC/17476 ciclos, IRQ serial = IF bit 7 em `rt_irq_service`, acesso byte/16/32 bits).
- **Duracao da troca**: 72 bits x baud = 10 486 ciclos a 115200 (< 17476 do Timer3: termina antes do estouro). ~16 trocas por quadro (280896/17476); um pacote gLink = 15 trocas, ou seja **1 pacote por quadro cabe** (ate o limite do wall-clock: troca TCP em localhost e ~sub-ms; o encontro mestre/filho espera ate 50 ms por ACK do filho, `KIRBY_LINK_NOACK=1` desliga).
- Transporte por troca mantido (funciona em localhost). Velocidade sem pacing ~6,5-7,4x tempo real com link ligado.
- **Acompanhamento no jogo** (script de teclas: `A@700+8,A@800+8,A@1060+8,DOWN@1200+4,A@1260+8,A@1500+8,DOWN@1650+4,A@1700+8`): titulo -> file select -> game select -> SUB-GAMES -> Speed Eaters -> MULTI PLAYER -> tela **TRANSMITTING** ("Please connect the cable", host vira P1 "OK?", join vira P2). Esse caminho e o handshake **multiboot/KMB** (0x6200 do mestre, 0x0000 do filho, SDK MultiBoot), NAO o gLink (0xFEFE): nenhum pacote valido ali.
- **Pacotes gLink validos foram vistos** (uma execucao, com `A@2500+8` extra no "OK?", KIRBY_SIO_EX ligado): no host, `P1 valid=61 sync=61`, `P2 valid=23 sync=71` (soma 0xFFF1), depois o jogo mostra "Communication Error". Nao reproduzi de forma estavel: o resultado depende de quando cada instancia chega ao "OK?" (execucao nao deterministica: wall-clock no encontro por ACK + velocidade sem pacing). Outras execucoes ficam em "Please connect" no join e so veem a troca inicial (`P1 6200.., P2 0000..`).
- **NAO atingido**: lobby estavel nas duas instancias, pacotes validos medidos no join, inicio de partida com teclas do outro jogador. Bloqueios: (1) caminho do menu: o gLink (LinkStartTimer 0x081576A0 chamado do lobby 0x0801A688, ver P0b da Poppy) parece vir de outro menu (aventura 4 Kirbys / mirror), nao do SUB-GAMES; (2) corrida de inicializacao entre as duas instancias; (3) o join as vezes nao termina limpo (`timeout` mata o processo depois do ultimo log: fechamento de socket/saida a investigar).
- Proximo passo: pedir a Poppy o caminho de menu exato ate 0x0801A688 (ou usar `--peek` em 0x03002558) e fixar a partida das duas instancias por um barramento de sincronizacao de quadro (lockstep de partida) em vez de wall-clock.
- Diagnostico: `KIRBY_SIO_TRACE=N`, `KIRBY_SIO_EX=N` (+`KIRBY_SIO_EX_FROM=troca`), `KIRBY_SIO_PKT=N`, `KIRBY_LINK_NOACK=1`; `--peek ADDR,..` imprime palavras da RAM a cada `--snap`; `rt_sio_report` mostra validos/invalidos por slot.
- Suite completa (139 testes) verde e quadro final 3000 frames mode=off md5 0455812b... identico.

### Cabo link P2a: lockstep por quadro, deterministico (Nasus)
**Resultado:** duas instancias reais (`--link host` / `--link join`) passam pelo menu SUB-GAMES > Speed Eaters > MULTI PLAYER, fazem o handshake de multiboot do Kirby (0x6200/0x81xx/0x8F51/0x70AE/E4E4), entram no gLink (0xFEFE) e **chegam a partida de Speed Eaters**, sem "Communication Error": 5200 quadros, **P1 valid=2673 / P2 valid=2672 / invalid=1 cada** (so o primeiro pacote, de aquecimento), identico nas duas instancias. O A do host inicia a partida nas duas telas; o A do join aparece no host (X de falsa largada no Kirby amarelo = P2). Antes (relogio real) so havia P1 valido e o resultado variava de execucao para execucao.
**Modelo (`LinkTransport.lockstep`, `frame_begin`/`frame_end`; `rt_sio_frame_end` e chamado por `grab_frame` a cada quadro do hw):**
- O host e a referencia. Quadro N: host executa N inteiro, entrega as trocas que fez (`LinkRound` = deslocamento em ciclos desde o inicio do quadro + 4 halfwords) e **espera o filho terminar o N**; so entao comeca o N+1. O filho espera `M_FRAME(N)`, executa o N reproduzindo as trocas do mestre nos mesmos deslocamentos (inicia a troca, IRQ serial ao fim, como no hw) e responde `M_DONE(N)`. Nenhuma instancia executa o quadro N+1 antes de a outra terminar o N (barreira por quadro do hw, vale sem VBlankIntrWait).
- **SEND do filho (o ponto delicado):** o gLink manda 1 halfword por TROCA (16 trocas/quadro: 0xFEFE + 14 + ociosa). O filho nao pode influenciar uma troca do quadro N que o host ja executou, entao o filho devolve o SIOMLT_SEND que tinha no INICIO de cada troca (`sends[]`) e o host os consome em ordem (FIFO) com **atraso fixo de `LINK_DELAY`=20 trocas** (~1,25 quadro; `KIRBY_LINK_DELAY=N`, faixa 0-100; >= max de trocas por quadro, que mediu 16). FIFO fixo (nao "o valor mais recente") mantem o fluxo de pacotes alinhado. Faltou dado (underflow) = repete o ultimo valor e conta (`[link] lockstep: N exchanges ran out of child data`); nos testes deu 0.
- Sem relogio real: o encontro por ACK de 50 ms foi substituido (so existe com `KIRBY_LINK_FREE=1`, modo antigo por troca, usado por `--selftest-link`). Setup: o host espera todos os jogadores (ate `KIRBY_LINK_WAIT_S`=120 s) e **fecha o listener** (quem chegar depois nao entra: o quadro dele nao bateria); o join tenta conectar (nao bloqueante, 250 ms por tentativa) ate o mesmo limite, senao roda sem cabo.
- **Pacing:** igual a antes (60 fps pelo relogio de audio/QPC em cada instancia; o filho acompanha o host porque bloqueia no `M_FRAME`). `--link-nopace` desliga o pacing mesmo com janela (testes rapidos). Sem janela nunca houve pacing.
**Determinismo:** `KIRBY_SIO_LOG=arquivo` grava `quadro ciclo-no-quadro s0 s1 s2 s3` de cada troca concluida; `rt_sio_report` imprime `LINK exchange hash` (FNV-1a de quadro, ciclo e 4 halfwords). Resultado: duas execucoes com o mesmo roteiro = mesmo hash (`3f70b1ec58b17557`, 11397 trocas, 3203 quadros) e host == join (mesmo hash e logs byte a byte iguais). Teste: `tests/test_link_lockstep.py` (RECOMP_SLOW=1): 2 execucoes x 3700 quadros, hash igual, host==join, P1 e P2 validos > 500 nas duas instancias. Constante `KEYS_COOP` e o gancho do roteiro do lobby do co-op da Poppy (ainda nao entregue; hoje o teste usa o roteiro SUB-GAMES acima).
**Trocas por quadro vs pacote de 24 B por quadro (para a P2/rede escolher o formato):** decidi **trocas (halfwords) agrupadas por quadro**, nao um pacote de 24 B. Motivo: ate chegar ao gLink o jogo usa handshakes de multiboot (0x6200, 0x8101+n, 0x8F51, 0x70AE, E4E4, 1 troca por quadro) que nao tem formato de pacote; o gLink so comeca depois. Mensagens por quadro (400 B hoje, fixas, comprimiveis): host->filho `M_FRAME` = ate 32 trocas (deslocamento u32 + 4 halfwords) + mascara de presentes; filho->host `M_DONE` = ate 32 halfwords (SEND no inicio de cada troca). Em regime gLink sao 16 trocas = **32 B por jogador por quadro** (16 halfwords) mais 16 deslocamentos; os deslocamentos sao periodicos (Timer3 17476 ciclos) e podem virar delta u16 ou ser omitidos. Para a rede: o lockstep puro exige 1 RTT por quadro; para latencia > ~16 ms o caminho e deixar o host adiantar K quadros (atraso de entrada, como o proprio jogo ja faz com os 16 quadros de espera) - nao implementado.
**Fechamento do join:** a causa era a reconexao: depois que o host saia, o filho tentava `connect` a cada 64 polls e cada tentativa recusada em localhost custa ~1 s no Windows (parecia travado, `timeout` matava). Agora o filho nao reconecta; quando o host cai, o filho segue sem cabo (`[link] host disconnected`) e sai normalmente (teste: host 900 quadros, join 1800 -> join sai com rc=0 em ~11 s).
**Sem mudanca de comportamento fora do link:** mode=off md5 do quadro final (3000 quadros) igual (0455812b...); `--selftest-link` OK (hub e TCP no modo antigo); suite (159 testes com RECOMP_SLOW=1) verde.

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
- `0x0809DA30..0x0809DA48`: o indice vem de um byte do objeto (`obj+0x82`) menos 5; a funcao testa
  `indice <= 0xD5` e salta pela tabela (`mov pc, ...`) => o limite real e `0xD5`, ou seja **214 entradas**. `jumptables.csv` registrou **64**: o
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

## Launcher `KirbyRecomp.exe` e pacote de release (Riven) - primeira versao
**Pecas** (`launcher/`, `tools/package_release.py`):
| Arquivo | Papel |
|---|---|
| `launcher/stub.c` -> `KirbyRecomp.exe` | stub Win32 minimo (subsystem windows, sem console): executa `python\pythonw.exe app\launcher\launcher.py <args>`; `--console` usa `python.exe` |
| `launcher/launcher.py` | arrastar/escolher ROM (dialogo Win32 via ctypes), SHA-1 (so USA), copia os fontes para `%LOCALAPPDATA%\KirbyRecomp\work`, roda `tools/build_all.py` lendo o progresso, abre `kirby.exe` |
| `launcher/ui_dll.c` -> `kirby_ui.dll` | fachada de `hw/window_sdl.c` (janela de progresso da Gwen) chamada por ctypes |
| `tools/package_release.py` | monta `dist/KirbyRecomp/` (python embutido + capstone, fontes, zig, SDL2), compila stub e DLL com o proprio zig, **audita** e falha se entrar ROM/save/C gerado/exe do jogo |
**Camadas:** `KirbyRecomp.exe` -> Python embutido (so stdlib + capstone) -> `build_all.py` -> `recomp.program`
-> zig compila o C gerado -> `kirby.exe` em `%LOCALAPPDATA%\KirbyRecomp\<sha1>`. Cache valido enquanto o
carimbo dos fontes nao muda (`build.ok`).
**Armadilhas achadas ao empacotar:**
1. `recomp/harness.py` importava `unicorn` (oraculo de teste) no topo: o build nao funciona sem ele. Agora e
   import opcional (so os testes precisam).
2. O Python *embeddable* **ignora `PYTHONPATH`** e o diretorio do script/`-m`. Solucao: `import site` no `_pth`
   + `Lib/site-packages/kirby_work.pth` que insere `$KIRBY_WORK` em `sys.path`.
3. Link da DLL: usar a import lib `libSDL2.dll.a`, nao a estatica (mesma regra do `program.py`).
4. `program.py` procurava o mapa em `<out>/analysis/`; `build_all.py` agora passa `--functions` explicito.
**Tamanho (sem compactar):** python 30 MB, sdl2 21 MB, **zig 361 MB** (o resto do app e pequeno): 417 MB. O zig e
o item a enxugar (so ha necessidade de `x86_64-windows-gnu`: libc mingw, headers, compiler-rt).

### Teste de instalacao limpa do launcher (Riven)
Pacote montado por `tools/package_release.py`, executado com ambiente vazio (`env -i`, PATH so com
System32, `LOCALAPPDATA` vazio), caminho com espacos (`Kirby Recomp Test`), ROM com outro nome
(`Kirby & The Amazing Mirror (USA).gba`), sem Python do sistema e sem `.venv`:
| Item | Resultado |
|---|---|
| 1a execucao (`KirbyRecomp.exe ROM --build-only`) | **6 min 04 s**, saida 0 |
| etapas | analise 14 s, indiretos 6 s, confianca 6 s, analise 2a passada 17 s, **compilar 321 s** |
| `[unresolved]` no log | **0** (as tabelas de ponteiros ja entram na analise; nao ha rodadas extras) |
| 2a execucao | **0,27 s** (cache valido, carimbo dos fontes) |
| jogo gerado, headless 2600 quadros + roteiro de teclas | `RT_STOP frames`, `nao_resolvidos=0`, quadro 2000 com `DISPCNT=1B40` (1a fase) |
- Compilar levou 321 s contra 146 s medidos pelo Nasus: aqui o zig estava **sem cache global** (1o uso
  compila compiler-rt etc.), em ambiente limpo, com 8 jobs. Em PCs com menos nucleos esperar mais.
- Pacote **enxuto** (zig reduzido a `x86_64-windows-gnu`): 347 MB sem compactar (zig 291 MB, python 30 MB,
  sdl2 21 MB). O `zig.exe` sozinho tem 177 MB. Zip ainda nao medido.
- Pendente: teste interativo (arrastar a ROM, ver a janela de progresso), zip final, avisos de licenca
  (zig MIT, SDL2 zlib, Python PSF, capstone BSD) em `README.txt`, e assinatura do exe (antivirus).
- **Pacote enxuto, mesmo teste limpo:** 4 min 32 s no total (compilar 233 s), `kirby.exe` gerado, saida 0.
  Zip do pacote enxuto: **95 MB** (347 MB sem compactar). Nao ha nada do jogo dentro (auditoria passa).

### Jogar enquanto compila (Riven) - launcher v0.2
Primeira abertura sem build compilado: o launcher gera um `kirby.exe` so com runtime + SDL + interpretador
(`recomp.program --interp-only`, ~1-2 s com cache do zig; **58 s com zig sem cache**), abre o jogo e dispara a
construcao completa como processo separado (`launcher.py ROM STAMP --bg-build`: destacado, prioridade
abaixo do normal, `cpu-2` jobs, `build.lock` com o PID). Ela sobrevive ao fechar da janela do jogo e grava
`build.ok`; na proxima abertura usa o compilado.
| Medida (ambiente limpo, zig frio, pacote de release) | |
|---|---|
| jogo (interpretador) na tela | **58 s** (usuario com zig quente: 10-15 s) |
| construcao completa em segundo plano | 273 s desde o inicio (4 jobs) |
| matar so a janela do jogo | a construcao continua e termina |
| 2a abertura (`--build-only`) | **215 ms** |
Bug achado pelo usuario: a janela de progresso aparecia como "Nao esta respondendo" porque a construcao do
interpretador era chamada bloqueante (sem bombear eventos). Corrigido (`build_interp` com Popen + `ui.update`
a cada 0,2 s). **Nao feito:** trocar do interpretado para o compilado na mesma sessao (DLL no mesmo processo).

## Launcher completo: tela de configuracao e botao Play (Riven) - plano e contrato (fase 0)
Decisoes do usuario: (1) a tela do launcher aparece **sempre** (com opcao "abrir direto"; Shift forca);
(2) botoes do controle mapeados por **posicao fisica**, nao por letra; (3) **toda a interface em ingles**,
incluindo os textos de progresso da construcao; (4) sem gerenciar o save por enquanto; (5) **F1** volta ao
launcher durante o jogo; Esc sai; F11 alterna tela cheia. A tela de controles deve lembrar a do yuzu:
**contorno do controle com os botoes** (desenho proprio, vetorial, em SDL; nenhum asset da Nintendo).

### Layout da tela (estilo retro, SDL, na DLL do launcher)
```
 KIRBY RECOMP                                         [ROM: ...(USA) ok]
 Status: Compiled version ready | Building 63% (you can play now) | Preparing...
 [Video]  Window size [1x 2x 3x 4x 5x 6x]  Fullscreen [ ]  Integer scaling [x]
 [Audio]  Volume [=====-----]  Mute [ ]
 [Input]  Keyboard... | Controller... | Reset to defaults
                          [ PLAY ]          [ Quit ]
```
Tela de controles (mesma area, abas Keyboard / Controller):
```
        [L]_______________________________[R]          <- ombros
   +--------------------------------------------+
   |   (D-pad)      +----------------+   (B) (A)|    <- GBA: B a esquerda e abaixo de A
   |    + cross     |   240x160      |           |
   |                +----------------+           |
   |           (Select) (Start)                  |
   +--------------------------------------------+
 clique/Enter num botao -> "Press a key/button for A..." (Esc cancela, conflito pede troca)
```
Desenho: corpo arredondado (retangulo com cantos), cruzeta, dois circulos A/B, pilulas Select/Start, dois
ombros L/R. Se o vetor ficar feio, plano B: os mesmos botoes na MESMA posicao, sem corpo.

### `kirby.ini` v2 (uma unica fonte; launcher e `kirby.exe` leem o mesmo)
```
[video]       scale=3   fullscreen=0   integer_scale=1   vsync=0
[audio]       volume=100   mute=0
[keyboard]    a=X  b=Z  l=A  r=S  start=Return  select=Backspace  up=Up  down=Down  left=Left  right=Right
[controller]  a=b  b=a  l=leftshoulder  r=rightshoulder  start=start  select=back
              up=dpup  down=dpdown  left=dpleft  right=dpright  stick_as_dpad=1  deadzone=8000
[launcher]    show=1   language=en
```
- Nomes de tecla = nomes do SDL (`SDL_GetKeyName`/`SDL_GetScancodeFromName`); botoes do controle = nomes de
  `SDL_GameControllerGetStringForButton` (`a,b,x,y,leftshoulder,start,back,dpup...`).
- **Por posicao** (SDL_GameController usa a posicao Xbox): GBA A -> botao leste (`b` do SDL); GBA B -> sul (`a`).
  No Switch Pro isso cai nos botoes rotulados A e B; no teclado a posicao vira X = A, Z = B (como o mGBA);
  hoje o projeto usa Z = A, X = B, entao muda.
- **Reservadas** (nao remapeaveis): Esc (sair), F11 (tela cheia), F1 (voltar ao launcher).

### Contrato launcher <-> DLL de UI (`kirby_ui.dll`, escrita em C por Gwen)
```
typedef struct { int state; int percent; char text[96]; char rom[260]; int rom_ok; } KlStatus;
/* state: 0 none, 1 preparing fast mode (Play disabled), 2 fast mode ready + full build running,
          3 compiled ready, 4 error */
typedef void (*KlPoll)(KlStatus *out);        /* called by the UI loop ~10x/s, from the UI thread   */
int kl_run(const char *ini_path, KlPoll poll);/* modal loop; returns KL_PLAY=1 | KL_QUIT=0 | KL_PICK_ROM=2 */
```
O launcher (Python, ctypes) chama `kl_run`; em `KL_PLAY` inicia o melhor `kirby.exe` (compilado se pronto,
senao interpretador) e **volta a `kl_run` se o jogo sair com codigo 77** (F1). A janela de progresso atual
deixa de existir: o progresso vira o campo `percent/text` da propria tela.

### Entrada unificada (`hw_input`, Gwen/Nasus)
Teclado + controle -> bits de `KEYINPUT`; hot-plug (`SDL_CONTROLLERDEVICEADDED/REMOVED`), zona morta, analogico
como direcional opcional, captura de tecla/botao para o remapeamento; mesma configuracao no launcher e no
`kirby.exe`. `kirby.exe` sai com **codigo 77** ao apertar F1.

### Fases, donos e criterio de pronto
| Fase | Entrega | Dono | Pronto quando |
|---|---|---|---|
| 0 | este contrato + `kirby.ini` v2 (leitura/escrita/validacao) | Gwen + Riven | teste de round-trip do ini |
| 1 | tela com status, video, audio, **Play/Quit**; ingles; progresso embutido | Gwen (UI) + Riven (fluxo) | build limpo abre a tela, Play inicia o jogo, F1 volta |
| 2 | remapeamento de teclado com desenho do controle | Gwen | remapear e persistir; conflito tratado |
| 3 | **controle**: leitura, mapeamento por posicao, navegacao da tela pelo controle, `kirby.exe` o usa | Gwen + Nasus | jogar e navegar so com o controle |
| 4 | acabamento: acentos desnecessarios (so ingles), "show launcher", aviso de teclas reservadas, Reset | todos | checklist de QA |
| QA | testes automatizados (SDL `dummy`, eventos simulados), roteiro manual | Poppy | suite em `tests/` |
**Riscos:** fonte 5x7 sem acentos (nao e problema em ingles); controles que o SDL nao reconhece (captura
manual); DPI/escala do Windows; foco da janela; fronteira Python<->C (a UI roda o laco, Python so responde ao
`poll`); `kirby.exe` como app grafico nao imprime no console (logs vao para arquivo).

### Ajustes do launcher pedidos pelo usuario (Riven) - decisoes
Feedback com capturas (tela cheia 1903x1079 mostrava o jogo em 1440x960 = 6x inteiro, bordas nos dois eixos):
- **Escala**: tres modos em `[video] scaling=integer|fit|stretch`; **padrao = fit** (decisao do usuario).
  `integer_scale=1` (v1/v2) -> integer; `integer_scale=0` -> fit. Fit mantem 3:2 (borda so nas laterais em 16:9);
  stretch preenche a tela e distorce; integer = pixels perfeitos com bordas.
- **Window size** desabilitado com Fullscreen marcado.
- **Launcher reflete as opcoes**: janela = 240*scale x 160*scale, **minimo 480x320**; em 1x a UI aparece em 2x com a
  legenda "1X PREVIEW (UI AT 2X)" (decisao do usuario); fullscreen aplica ao launcher.
- **Select ROM** so aparece quando nao ha ROM carregada; com ROM mostra apenas "ROM: <nome> OK".
- Donos: Gwen (UI + `config_ini`), Nasus (`kirby.exe` usa o MESMO parser e o modo de escala), Poppy (QA).

### Fases 2 e 3 (editar entradas): decisoes do usuario (Riven)
- **Um unico controle** (o primeiro disponivel; se desconectar, passa ao seguinte). Sem multiplayer.
- **Analogico utilizavel**: o analogico esquerdo age como direcional (`stick_as_dpad=1`, padrao ligado), com
  **zona morta ajustavel** (`deadzone`, 0-32767, padrao 8000). Na aba Controller: checkbox "Left stick as D-pad",
  slider "Deadzone" e um **indicador ao vivo** da posicao do analogico (ponto dentro de um circulo com a zona morta
  desenhada) para o usuario calibrar. 8 direcoes (diagonais pelas combinacoes), histerese para nao oscilar na borda.
- Teclas reservadas (Esc, F11, F1) nao sao remapeaveis; troca ao detectar conflito; Reset por aba.
- Desenho do controle: vetorial proprio (corpo, L/R, cruzeta, A/B, Select/Start), sem assets da Nintendo.

### Controle fisico testado (usuario)
Testado com **um controle generico**: funciona (leitura por posicao, analogico, remapeamento). Modelos Xbox, PlayStation e
Switch Pro seguem sem teste proprio; dependem dos mapeamentos do SDL_GameController.

## Cache do zig no pacote e nomes invalidos no .ini (Riven)
**Nomes invalidos**: `rt_input_init` agora cai no padrao (e loga `unknown key name 'X' ... using the default 'Y'`) quando o SDL
nao conhece o nome de tecla/botao digitado a mao; os 2 `expectedFailure` do QA viraram testes normais (80 passam).
**Cache do zig empacotado** (`package_release.py`, passo 4b; launcher `zig_cache_dir()`):
| Medida (modo rapido, layout do launcher) | Tempo |
|---|---|
| cache vazio | 76,6 s |
| cache empacotado (copia por Python) | **5,9 s** |
| launcher completo, ambiente limpo: janela do jogo no ar | **14 s** (antes 58 s); inclui copiar o cache (7 s) |
| construcao completa em segundo plano | pronta em 234 s desde o inicio; 2a abertura 275 ms |
Tres armadilhas medidas:
1. `-target native` prende o cache a CPU de quem compilou: o build passou a usar `-target x86_64-windows-gnu`
   (`KIRBY_ZIG_TARGET=native` restaura). Custo: ~507 q/s (8,5x tempo real) contra ~565-590 (9,5x) antes.
2. O cache do zig classifica arquivos por **prefixo de caminho** (cwd / lib dir / cache dir). Aquecer o cache com cwd =
   raiz do repo (que contem `dist/.../zig/lib`) gravou prefixos que nunca casam no launcher: 127 s em vez de 6 s.
   O aquecimento agora roda numa copia dos fontes FORA da arvore do zig, igual ao launcher.
3. O launcher so usa a UI falsa quando `KIRBY_FAKE_UI` esta definido (antes, dependia de a DLL nao ter `kl_run`).
Zip: 95 -> 122 MB. Cache enviado: 76 MB (copiado para `%LOCALAPPDATA%\KirbyRecomp\zigcache` na 1a abertura).
Medicoes anteriores de 183 s e 529 s foram invalidas (dois testes concorrentes na mesma pasta).

## Gerenciamento de save: plano e contrato (Riven)
Decisao do usuario: fazer (1) gravar ao escrever + atomico, (2) backups automaticos, (3) abrir pasta, exportar/importar `.sav`.
Sem save states (a pilha nativa do C impede congelar em qualquer ponto).
- **Arquivo do save**: `<pasta da ROM>/<nome da ROM>.sav`, cru, **32768 bytes** (SRAM, formato dos outros emuladores).
- **Runtime (Nasus)**: marca a SRAM como "suja" a cada escrita do jogo; um *debounce* de ~1 s grava (flush) em
  `<save>.tmp` e **renomeia** sobre o `.sav` (MoveFileEx com REPLACE_EXISTING = atomico); tambem grava ao sair (atexit),
  ao F1 e ao fechar a janela. Nunca grava um arquivo parcial. Falha de gravacao: avisar 1x no log, tentar de novo.
- **Launcher (Riven, Python)**: antes de iniciar o jogo copia o `.sav` para
  `%LOCALAPPDATA%\KirbyRecompackups\<sha1 da ROM>\<nome>_AAAAMMDD_HHMMSS.sav` se ele difere do backup mais recente;
  guarda os **10 mais recentes**. Acoes: abrir pasta do save (`os.startfile`), exportar (GetSaveFileName), importar
  (valida tamanho 32768, avisa se vazio/tudo 0xFF, **faz backup antes** de sobrescrever).
- **UI (Gwen)**: secao "SAVE" na tela principal com tres botoes: OPEN FOLDER, EXPORT..., IMPORT... Novos retornos de
  `kl_run`: KL_OPEN_SAVE_DIR=3, KL_EXPORT_SAVE=4, KL_IMPORT_SAVE=5 (o launcher trata e chama `kl_run` de novo). Botoes
  desabilitados se nao houver ROM; IMPORT pede confirmacao ("OVERWRITE CURRENT SAVE? A BACKUP IS KEPT").
- **QA (Poppy)**: kill -9 durante a escrita nao corrompe; flush em ~1 s; backups rotacionam em 10; import de tamanho errado
  recusado; export/import round-trip identico; save com caminho com acento e espaco.

### Save completo (Riven) - launcher v0.5
`launcher/savemgr.py` (QA: `tests/test_save_qa.py`, 34 testes): `backup_save` (so se difere do backup mais recente; 10 por ROM em
`%LOCALAPPDATA%\KirbyRecompackups\<sha1>\<nome>_AAAAMMDD_HHMMSS.sav`, `_1`, `_2`... no mesmo segundo), `import_save` (valida
32768 bytes, avisa se vazio, faz backup antes, troca atomica por `os.replace`), `export_save`. `flow.Controller` trata os codigos 3/4/5 de
`kl_run` (abrir pasta / exportar / importar), executa a acao e volta a tela; uma acao que falha nao derruba o launcher; codigo
desconhecido = sair. `LauncherEnv.run_game` faz o backup antes de cada Play. Dialogos Win32 de abrir/salvar via ctypes (o filtro
de tipos passou a ser um buffer proprio: antes o NUL embutido cortava a string no 1o separador).
Nota de teste: `tests.test_save` procura `build/recomp/program/rt_boot.exe`; um build de desenvolvimento antigo faz esses 2 testes
falharem (nao e defeito do gravador). Defina `KIRBY_RT_BOOT` com um `rt_boot.exe` recente (`recomp.program --interp-only`).

## Online / cabo link: plano (Riven, 2026-10-16)

**Veredito:** viável, mas é a maior funcionalidade do projeto. Só funciona bem em LAN ou internet com latência baixa (< ~60 ms).

**O que o jogo usa:** modo *multi-play* do SIO (SIOCNT 0x04000128, SIOMULTI0-3 0x04000120-126, SIOMLT_SEND 0x0400012A, RCNT 0x04000134).
O mestre (jogador 1) inicia a troca; cada jogador envia 16 bits na sua vez; todos recebem os 4 valores; IRQ serial no fim.
Varredura da ROM: 35 funções tocam SIO/RCNT/JOY (SIOCNT/SEND/MULTI em ~0x08030xxx–0x08033xxx e ~0x08157xxx–0x08158xxx;
RCNT em 0x080001CC, 0x08019FDC, 0x08031D24, 0x0815158C, 0x08157210; JOYCNT em 0x08125B3C, 0x08125D60, 0x0812FA28).
Literais de SIOCNT que parecem multi-play: 0x2003 (115200 baud) em 0x08157700 e 0x08158194, 0x2002 em 0x081580F4 (a confirmar).
Os blocos de código copiados para RAM (0x03006C50, 0x030068E0) não têm literais de SIO.

**Abordagem:** *lockstep determinístico* por frame, não rollback (rollback exige save states, que não temos).
Cada instância emula o SIO localmente; a rede só troca o valor de 16 bits de cada jogador por troca (e, por frame, um hash de estado para detectar dessincronização).
Todos os pares precisam usar o mesmo build (compilado ou interpretado) porque o timing é aproximado.

**Fases**
- P0 (estudo, Poppy + Riven): ler as ~35 funções, descobrir quantas trocas por frame, qual IRQ, o que acontece sem cabo/timeouts. Saída: documento do protocolo do jogo.
- P1 (Nasus + Gwen): emular SIO multi-play em hw/ com N instâncias locais (loopback/pipe), mestre/filho, IRQ serial, SIOMULTIx. Teste: 2 e 4 instâncias na mesma máquina.
- P2 (Riven): transporte TCP/UDP em LAN, lockstep com buffer de atraso, medição de latência, hash por frame, detecção de dessincronização.
- P3 (Riven): internet: IP direto + porta; depois relay/código de sala, se valer a pena.
- P4 (Gwen + Riven): lobby no launcher (host/join, slots 1-4, status, ping).
- QA (Poppy): testes de desync, desconexão no meio do jogo, 2/3/4 jogadores.

**Riscos:** latência vs 60 fps; o jogo pode ter timeouts de link curtos; timing não é cycle-accurate; NAT; cada jogador precisa da própria ROM e save; multiboot (SWI 0x25) fora do escopo.

## Protocolo de link do KatAM (Poppy, P0 - estudo, 2026-10-16)
Fonte: disassembly das funcoes que tocam o link + `tools/link_functions.py` (gera `build/analysis/link_functions.csv`, 52 linhas:
endereco, camada, o que faz, registradores lidos/escritos, quem chama, ponteiros em dados, confianca). Nada do jogo foi alterado.
Confianca: **[L]** li o codigo inteiro e o comportamento esta confirmado; **[P]** li parte / deduzi pelo contexto; **[?]** nao verificado.

### 0. Correcoes ao plano (Riven) - o que mudou depois de ler o codigo
- **JOY bus NAO e usado.** Os "JOYCNT" achados (0x08125C78, 0x08125E70, 0x081303DC) sao `0x04000140` como **argumento de CpuSet**
  (copia de 0x140 palavras), nao o registrador. Nenhum literal de JOY_RECV/JOY_TRANS/JOYSTAT existe na ROM; zero acessos reais [L].
- **Dos "35": 32 funcoes tocam de fato SIO/RCNT** (acesso efetivo por `ldr/str` com endereco constante). 3 dos 35 eram falsos positivos
  (literal 0x04000128/0x04000134 usado como tamanho de CpuSet) e 4 achados do scan (0x080002C8, 0x08152388, 0x08152694, 0x08152790) so
  tocam KEYINPUT/KEYCNT/IE. Alem disso existe **codigo SIO fora do mapa**: um bloco Thumb de 1152 B em `0x08158A28-0x08158EA8`
  (biblioteca Normal-32 bits) classificado como "dados", **sem nenhuma referencia** (nem `bl` nem ponteiro de fora) = codigo morto [L].
- **`0x2003`** em 0x08157700 e 0x08158194: confirmado, mas NAO e a configuracao do jogo em partida: e **parar o link / iniciar transferencia
  em bloco** (secao 2). A configuracao de jogo e **`0x6003`**. **`0x2002`** em 0x081580F4 **nao e valor de SIOCNT**: e o limite de um laco (`cmp r1,#0x2002`) [L].
- O jogo tem **quatro camadas de link**, so a 1a e a 2a importam para jogar online (secao 1).

### 1. Arquitetura (4 camadas) e ordem por quadro
| camada | funcoes | papel | online? |
|---|---|---|---|
| **gLink** (motor de pacotes, estrutura `0x03006A60`) | `LinkInit 0x08157210`, `LinkMain 0x08157308`, `LinkBuildSend 0x08157464`, `LinkReceive 0x08157534`, `LinkStop 0x081576BC`, `LinkEnable 0x0815898C`, `LinkDisable 0x081589E8`, `LinkReset 0x08158934`; **IRQ em ARM na IWRAM**: `0x030068E0` (copiada de ROM `0x08158F54`, 384 B) e `0x03006C50` (de `0x08158F14`, 64 B) | transmite 1 pacote de 24 B por jogador por quadro sobre SIO multi-play | **sim** |
| **lockstep** (jogo) | `BuildInputPacket 0x08030E44`, `LinkFrameSync 0x08030FE0`, `LinkErrorScreen 0x08032E98` | todos os jogadores trocam as TECLAS de cada quadro; o jogo so avanca quando tem as de todos | **sim** (e a base do online) |
| **lobby** | `0x0801A618`, `0x0801A908`, `0x0803169C`, `0x080315B8`, `0x08031764/60/F0/A8`, `0x080323xx`, `0x080324BC`, `0x080332BC`, `0x0801F9FC` | tela "esperando jogadores": hello, escolha de personagem, pronto/iniciar; usa o MESMO motor gLink com pacotes de outro tipo | sim |
| **multiboot / bloco 32 bits** | `KMB_*` `0x0803024C-0x08030C40` (vetor Thumb em ROM `0x08030899`), SDK MultiBoot `0x0815770C`, `0x08157B54`, `0x08157CE0`, `0x08157E0C`, `0x08157E54`, `0x08158038`, `0x08158100`, `0x0801A6BC`, `0x08019FDC`, `0x08031D24` | "download play" (clientes sem cartucho recebem uma imagem) e envio de bloco de 32 KiB em SIO Normal 32 bits | **nao** (fora do escopo; so nao pode travar) |
Ordem em cada quadro (laco principal `0x08151C54`), com link ligado (`0x03002558 != 0`): `BuildInputPacket` -> `LinkMain(0x030036B0, 0x03002490, flag)`
-> `LinkFrameSync`; se esta retorna 0, `LinkErrorScreen`. Sem link: nada disso roda. `LinkEnable` liga (instala o vetor 0 = `0x030068E0`, `LinkInit(0)`),
`LinkDisable` desliga (`LinkStop`, `LinkInit(0)`). O estado do menu fica em `0x03002554` (status devolvido por `LinkMain`) [L].

### 2. Como o jogo configura o SIO (valores e onde)
| valor | onde | significado |
|---|---|---|
| `RCNT = 0` | `LinkInit 0x08157210`, `0x080001CC`, `0x08019FDC`, `0x08031D24` | modo SIO (bit15=0): multi-play/normal. **Nunca escreve outro valor.** |
| `SIOCNT = 0x2000` depois `\|= 0x4003` -> **`0x6003`** | `LinkInit`, `KMB_Open 0x0803024C` | **multi-play, 115200 baud, IRQ de serial ligada**. Escrita 32 bits em 0x04000128: SIOCNT e SIOMLT_SEND=0 de uma vez [L] |
| `SIOCNT = 0x2003` | `LinkStop 0x081576BC`; `BulkTransferInit 0x08158100` | multi-play 115200 SEM IRQ (parado/ocioso) [L] |
| `SIOCNT = 0x1000` depois `0x1001` | `0x08158100` (so se mestre), `0x08157E54` | **Normal 32 bits**, clock interno 256 kHz (transferencia em bloco; fora do escopo) [L] |
| `SIOCNT = 0x1008` `\|0x4000`, `RCNT=0`, `TM3CNT_H=0xC1` | bloco morto `0x08158A28` | Normal 32 bits, clock externo (escravo); nunca executado [L] |
| `SIOMLT_SEND` | handler `0x030068E0`; `0x08157B54`; `0x0803xxxx` | **0xFEFE** = sincronismo/ocioso do motor gLink; `0x8F50/0x8F51/0x8F52`, `0x70AE`, `0xE4E4` = handshake multiboot do Kirby; `0x6100..0x63xx` / `0x72xx` = handshake MultiBoot do SDK [L/P] |
| `TM3CNT_L = 0xBBBC`, `TM3CNT_H = 0xC0` | `LinkMain`, `LinkBuildSend`, handler | Timer3, prescaler F/1: estouro a cada **17476 ciclos = 1,0415 ms**; IRQ ligada. Cadencia do mestre (secao 5) [L] |
| `IE`: `&=0xFF3F` na init, `\|=0x80` no fim; no mestre `&=0xFF7F` e `\|=0x40` | `LinkInit`, `LinkMain` | filho: serial (bit7). **Mestre: serial desligada e Timer3 (bit6) ligado** (e `SIOCNT` bit14 limpo) [L] |
| vetor de IRQ 0 (`0x030017B0`) = `0x030068E0` | `LinkEnable`, `0x0801A908` | bits 6 e 7 do IF (Timer3 e serial) caem no MESMO slot 0 (o crt0 testa `IF & 0xC0` primeiro) [L] |
**Leitura de RCNT no boot** (`MainInit 0x0815158C`): `(RCNT & 0xC000) != 0x8000` -> `0x03002440 = 0x200` ("fui iniciado por MultiBoot como cliente").
Em console real, depois do boot da BIOS com cartucho, RCNT le **0x8000** -> **o hw tem que resetar RCNT para 0x8000** [L: o teste; P: o valor de reset da BIOS].

### 3. Quem e mestre e quem e filho
- **Decidido pelo hardware/cabo, nao pelo jogo**: o mestre e a unidade com SI=0 (bit 2 de SIOCNT) e ID=0 (bits 4-5); filhos tem SI=1 e ID 1..3 (valido depois da 1a troca).
- `LinkMain` (estado 0) so assume o papel de mestre quando `(SIOCNT & 0x30)==0` **e** `(SIOCNT & 0x88)==8` (SD=1 = todos prontos; nao ocupado) **e** `(SIOCNT & 4)==0` **e** `gLink[0x14]==-1`.
  Faz: IE serial off + Timer3 on, `SIOCNT` bit14 off, `IF=0xC0`, `TM3 = reload 0xBBBC`, `gLink[0]=8` (flag "mestre"), **`SIOCNT |= 0x80` (dispara a 1a troca)**, estado=1 [L].
  Filho (ID != 0): vai direto para o estado 1 e so responde as trocas do mestre [L].
- SD precisa ser 1 para o mestre comecar: **sem cabo/sem parceiros o mestre fica esperando para sempre no estado 0** (lobby mostra "esperando"). [L]
- Em estado 1 o mestre continua a cadeia sozinho: o handler (rodando no estouro do Timer3) faz `TM3CNT_H=0`, `SIOCNT|=0x80`, `TM3CNT_H=0xC0` (reinicia o relogio). [L]

### 4. Trocas por quadro, formato de 16 bits e o que o jogo faz com os 4 valores
Cada GBA envia **1 pacote de 12 halfwords (24 B)** por quadro (`LinkMain`). Em fio, a sequencia por pacote e: **1 troca de sincronismo (todos mandam 0xFEFE)** + 14 trocas
(`sendbuf[0..13]`, os 2 ultimos halfwords valem 0) = **15 trocas**. Com o Timer3 em 1,04 ms o mestre faz ~16,07 trocas por quadro (280896 ciclos / 17476), logo cabe 1 pacote por quadro
com folga de ~1 troca; depois do halfword 13 o mestre segue trocando (enviando `sendbuf[13]`=0) ate `LinkBuildSend` armar o proximo pacote (`gLink[0x14]=-1`). [L]
```
exchange 0      : todos enviam 0xFEFE            (receptor: se idx>11 e valor==0xFEFE -> idx=-1; depois idx=0)
exchange 1..12  : hw0 .. hw11 do pacote           (receptor guarda recv[p][idx]; no idx==11: troca os 2 buffers e gLink[4+p]=1)
exchange 13..14 : hw12, hw13 (zeros)              (sobram no buffer; ignorados)
hw0  = [byte0] contador de quadro gLink[0xB] ; [byte1] flags: bits0-3 = (conectados_alguma_vez XOR conectados_agora), bit4 = flag do LinkMain,
       bit5 = 0x03006C90[3]&1 (transferencia em bloco concluida), bit6 = gLink[8].bit6 (sincronia)
hw1  = checksum = ~(soma de hw0..hw11 com hw1=0) - 14        => a soma dos 12 halfwords recebidos tem que ser 0xFFF1 (-15)
hw2..hw11 = 20 bytes de payload (5 palavras copiadas de 0x030036B0)
```
- Pacote **invalido** (soma != -15 ou 0xFFFF de filho ausente): `LinkReceive` nao atualiza `recv_dest[p*20]` (fica o dado antigo) e limpa o bit de "conectado agora" do jogador. [L]
- Os 4 valores `SIOMULTI0-3` de cada troca vao um para cada jogador (indice = slot): o handler guarda `SIOMULTI[p]` no buffer do jogador `p`; **nao ha broadcast de dados "para todos" fora disso** — cada GBA monta o pacote de CADA um dos 4 a partir das 4 portas. [L]
- **Status devolvido por `LinkMain`** (vai para `0x03002554`): bits 0-3 = jogadores com pacote valido neste quadro; bits 4/5/6 = flags de sincronia vindas do cabecalho do jogador 0;
  bit 7 (0x80) = eu sou o mestre; bits 8-11 = jogadores conectados alguma vez (`gLink[2]<<8`); bit 12 = SIOCNT bit6 (erro de transmissao); bit 15 = contador de falha >= 8; bit 13 = ID invalido. [L para 0-3,7-12,15; P para 4-6]
- **Payload do jogo (tipo 0x20) = lockstep de teclas**: `[0]=0x20`, `[1]` = byte de sessao (`0x020382D0+6`), `[2..0xD]` = **8 entradas de 12 bits empacotadas LSB-first, a mais nova primeiro**
  (cada entrada = `KEYINPUT & 0x3FF` | checksum de 2 bits do estado do jogo << 10), `[0x10..0x13]` = numero do quadro (u32). Isto da redundancia de 8 quadros: um pacote perdido nao perde teclas. [L]
- O que `LinkFrameSync` faz com os valores recebidos: para cada jogador `p < N` (N = byte `0x0203AD30`, definido ao iniciar a sessao): o proprio slot (`ID == p`, de `SIOCNT>>4&3`) copia o anel local; os outros
  exigem tipo 0x20 e sessao igual, reconstroem o anel remoto (16 entradas x 8 B em `0x020382D0+0xA0+p*0x80`) com as 8 entradas (delta = quadro - ultimo, ate 8), comparam o checksum de 2 bits do estado
  (so se a flag de verificacao 0x8 esta ligada) e **so avancam o contador de quadro quando TODOS os jogadores tem a tecla do proximo quadro** (senao `flags bit2` = esperando) — e **um lockstep de verdade**. [L]
- Lobby (mesmo motor, outros pacotes): `byte0` = `2` hello (com `hw1=0x0123`, 16 B de dados do jogador e `byte1` = modo/versao), `byte 0xE` = fase `0x20/0x40/0x41/0x42/0x22` (escolha, pronto, iniciar, confirmar). Sobre quais bytes
  exatos cada fase leva: [P]. O `byte0` do payload no lobby vem de um halfword da ROM (`0x08D60A80`) [P].

### 5. IRQ serial e o handler
- IRQs usadas: **serial (IF bit 7)** nos filhos, **Timer3 (IF bit 6)** no mestre; ambas no vetor 0 -> handler ARM em IWRAM `0x030068E0` (nao roda da ROM: o runtime precisa tratar overlay de codigo em RAM; ver `ram_code.csv` blocos 1 e 2). [L]
- Handler, ordem exata: le SIOMULTI0-3 (2 `ldm` de 2 palavras: 0x04000120 e 0x04000124); `gLink[9] = SIOCNT & 0x40`; **envio**: `idx=gLink[0x14]`; `idx==-1` -> `SIOMLT_SEND=0xFEFE`, troca os 2 buffers de envio,
  `idx=0`; `idx>=0` -> `SIOMLT_SEND = sendbuf[idx]`; `idx++` ate 13; **recepcao** p=3..0 (veja secao 4); se mestre: `TM3CNT_H=0`, `SIOCNT|=0x80`, `TM3CNT_H=0xC0`, `IF` ack. [L]
- O estado do hardware que o handler assume: ao ser chamado, **a troca anterior ja terminou** (SIOMULTI validos); a nova troca so e iniciada por ele (mestre). Trocas nunca se sobrepoem. [L]

### 6. Timeouts, cabo desconectado, jogador que cai
| situacao | o que o jogo faz | prazo |
|---|---|---|
| filho ausente / cabo solto | SIOMULTIn = 0xFFFF -> pacote invalido (soma != -15); bit do jogador sai de "conectado agora"; vira bit no nibble do cabecalho (visivel a todos no quadro seguinte) | 1 quadro |
| motor gLink sem sincronia do mestre | contador `gLink[0xA]` +1 por quadro sem a flag de sincronia (bit6); **> 7 (8 quadros)** -> estado 2 e status bit15 | 8 quadros (acumulado, nao zera) |
| faltam as teclas de um jogador (queda, rede lenta) | `LinkFrameSync` NAO avanca o jogo (espera); erro **6** (buraco/0xFFFF) ja no 1o quadro sem pacote valido. **CORRIGIDO (execucao, secao 12)**: o erro 7 (espera > 16 quadros) so existe se a "profundidade alvo" `0x020382D0+0x2A0+p` for >= 2; no estado padrao e 0 e o erro visto numa queda e o **6** | 6: imediato; 7: ~16 quadros (~267 ms) |
| pacote com tipo != 0x20 / sessao diferente / anel estourou / marcador 0xFFFF | erros **3 / 4 / 5 / 6** | imediato |
| checksum de estado de jogo diferente entre jogadores | erro **8** = **dessincronia detectada pelo proprio jogo** (2 bits por quadro, so se flag 0x8) | 1 quadro |
| qualquer erro | `LinkErrorScreen 0x08032E98`: mostra tela de erro com o codigo (`0x02038580`), `LinkDisable`, limpa buffers e volta ao menu | — |
Conclusao: o jogo ja tolera **latencia/perda de ate ~15 quadros** por jogador sem erro (as teclas sao redundantes em 8 quadros e ele espera), e **detecta dessincronia sozinho**. O mestre nunca muda: se o mestre cai, a sessao acaba (erro 6, ou 7 se a profundidade alvo >= 2). [L, corrigido na secao 12]

### 7. Quantos jogadores
- A deteccao e por **pacotes validos**, nao por SIOCNT diretamente: `gLink[3]` (agora) e `gLink[2]` (alguma vez) por slot; o lobby passa `(status >> 8) & 0xF` ao `LinkInit` e ao iniciar a sessao fixa
  `N = 0x0203AD30` e `meuID = 0x0203AD3C` (de `SIOCNT>>4&3`). Maximo 4 (todos os lacos vao ate 3). [L]
- SIOCNT: bit3 **SD** = "todos os GBAs em multi-play e prontos" (condicao para o mestre comecar); bit2 **SI** = 0 so no mestre; bits4-5 **ID** = slot proprio (so valido depois da 1a troca); bit6 = erro;
  bit7 = ocupado/inicio. Valor **0xFFFF** em SIOMULTIn = filho nao respondeu; **0xFEFE** e o marcador do protocolo (nao e "ausente"). [L]

### 8. JOY bus
Nao ha nenhum acesso a JOYCNT/JOY_RECV/JOY_TRANS/JOYSTAT. Confirmado por: (a) zero literais de 0x04000150/154/158 na ROM; (b) o unico 0x04000140 (3 usos) e tamanho de CpuSet; (c) nenhum acesso por endereco efetivo. **O hw/ nao precisa de JOY.** [L]

### 9. Minimo que o hw/ precisa emular (para o jogo entrar no menu multiplayer e jogar)
Estado em 2026-10-16 do `hw/hw.c` + `rt_sio.c` (Nasus/Gwen, P1) marcado entre parenteses; **[V]** = verificar, nao consegui confirmar so lendo.
1. **RCNT** 0x04000134: R/W 16 bits, **reset = 0x8000** (senao `0x03002440 = 0x200` e o jogo age como cliente MultiBoot). [V: nao vi o reset no hw.c]
2. **SIOCNT** 0x04000128: leitura/escrita 16 e **32 bits** (o jogo le `ldr r6,[SIOCNT]` = SIOCNT | SIOMLT_SEND<<16 e escreve `str 0x2000` seguido de `strh`), tambem **escrita de BYTE** em `[SIOCNT]` e `[SIOCNT+1]`
   (`LinkMain` faz `strb` com `\|=0x80` e `&=0xBF`) — as duas precisam chegar ao tratador de SIOCNT. Bits: 0-1 baud, 2 SI, 3 SD, 4-5 ID, 6 erro, 7 ocupado/start, 12-13 modo (multi = 10), 14 IRQ. (hw.c: ja trata multi, start so do mestre, SI/SD/ID/erro/ocupado) [V: byte/32 bits]
3. **SIOMLT_SEND** 0x0400012A R/W; **SIOMULTI0-3** 0x04000120-26 somente-leitura em multi-play, legiveis como pares de 32 bits (`ldm`). (hw.c: sim)
4. **Troca multi-play** iniciada so no mestre por `SIOCNT |= 0x80` (SD=1): coleta SIOMLT_SEND de cada unidade (0xFFFF para ausente), entrega os 4 valores em todas, limpa ocupado, ID valido, IRQ se bit14. (hw.c: gancho `start`/`deliver`)
5. **IRQ de serial (IF bit 7)** e **Timer3 com IRQ (IF bit 6)**: prescaler F/1, recarga de `TM3CNT_L=0xBBBC` em escrita de 32 bits em 0x0400010C (L|H<<16) e reinicio ao escrever `TM3CNT_H`. O mestre depende SO do Timer3. [V: timers de hw/]
6. **SD/SI por papel**: o host (slot 0) tem SI=0; so poe SD=1 quando todos os slots estao presentes.
7. **Sem cabo** (entrar no menu): o mestre fica no estado 0 para sempre (SD=0): nada trava; o menu mostra "esperando". Uma troca iniciada sozinha termina com erro (hw.c: ja faz). [L]
8. **Codigo em IWRAM executando em IRQ** (`0x030068E0`/`0x03006C50`): o despacho de IRQ do runtime tem que conseguir chamar codigo de overlay RAM no vetor 0. [V: runtime]
9. O que NAO precisa: JOY, SIO Normal 32 bits, MultiBoot (SWI 0x25), UART, modo geral (GPIO), SD/SO/SC como pinos.
**Cadencia:** as 15 trocas de um pacote devem caber em ~1 quadro. `hw_sio_slot_cycles` = 18 bits por unidade (~2621 ciclos a 115200 baud) => 4 unidades ~10,5 k ciclos < 17476 (periodo do Timer3): entregar a troca ANTES do estouro, senao o handler le SIOMULTI da troca anterior.

### 10. Implicacoes para o online (inferencia, nao verificado em execucao)
- O protocolo do jogo ja e **lockstep por teclas com redundancia de 8 quadros e espera ate 16**: nao e preciso rede no nivel de cada troca de 16 bits (impossivel a 1 ms).
  Plano coerente: cada instancia emula o SIO localmente e a rede leva, por jogador, **o fluxo de halfwords do pacote (24 B por quadro)**; o SIO local entrega aos handlers o ultimo pacote conhecido de cada remoto,
  alinhado pelo marcador 0xFEFE; pacote atrasado = 0xFFFF naquela troca (invalido) e o jogo espera. Tolera atraso de pacote enquanto as 8 teclas redundantes cobrirem (a espera de 16 quadros e o erro 7 dependem da profundidade alvo; veja secao 12).
- Todos precisam do mesmo codigo/estado inicial e do mesmo byte de sessao `0x020382D0+6`; a dessincronizacao e detectada pelo proprio jogo (erro 8) se a flag de verificacao estiver ligada.
- Risco: o mestre faz ~16 trocas por quadro **dirigidas pelo Timer3**; se o hw emulado entrega as trocas mais rapido/lento que 1,04 ms por troca, o pacote pode nao completar em um quadro (o receptor so completa em idx==11).

### 11. Pendencias deste estudo (atualizado em P0b; ver secao 12)
- Fases/bytes de lobby 0x20/0x40/0x41/0x42/0x22 e bit19 de `0x03002440`: decodificacao estatica na secao 12; a confirmacao com TRACOS das 2 instancias reais depende do formato pedido no ROLES.md.
- Semantica fina das flags de cabecalho bits 4-6: ainda so por leitura de codigo.
- Secao 4 (15 trocas, soma -15) **CONFIRMADA em execucao** (secao 12).

### 12. P0b: execucao do codigo real do link (Poppy, 2026-10-16)
**Bancada** `tools/link_sim.py` (Unicorn, so no .venv): carrega a ROM, roda o codigo REAL `LinkEnable/LinkInit/LinkMain`, o handler ARM de IWRAM (copiado de 0x08158F54), `BuildInputPacket` e `LinkFrameSync`, ligando N "GBAs" por um cabo multi-play em Python (SIOMULTI, SIOCNT, Timer3, IRQ serial/Timer3 pelo vetor 0, CpuSet por HLE). Cada halfword enviado/recebido fica em `Session.cable.log`. NAO e emulador de GBA (so o que o link toca).
**Testes de aceite**: `.venv\Scripts\python.exe -m unittest tests.test_link_protocol -v` (18 testes, ~6 s, sem rede; pulam sem unicorn/ROM/exe).
| verificacao | resultado |
|---|---|
| soma dos 12 halfwords (hw0..hw11) | == 0xFFF1 em TODO pacote enviado, 2/3/4 jogadores, mestre e filhos |
| ordem | 1 troca `0xFEFE`, depois hw0..hw13 (hw0 = contador de quadro | flags<<8, hw1 = `~soma-14`, hw2..hw11 = 20 B de payload, hw12 = hw13 = 0) = **15 trocas por pacote**, mais **1 troca ociosa 0x0000** = **16 trocas por quadro** (mestre: `0xFEFE` a cada 16). Filhos: o `0xFEFE` sai repetido (atraso de 1 troca); vale o ultimo |
| contador de quadro | sobe 1 por pacote |
| jogador ausente | slot do cabo = 0xFFFF em toda troca; o bit do jogador sai do status (`0x797 -> 0x793` para o jogador 2); a copia do jogador na area de recepcao (`0x03002490+20*p`) **nao e atualizada** (pacote descartado) |
| link off | `rt_boot.exe` 400 quadros: sem flag == `--link off` == `--link solo` (md5 do PPM igual) |
**Achado de bootstrap**: o mestre so encadeia as trocas do Timer3 depois de `LinkStartTimer` (0x081576A0, `gLink[8] |= 0x80` se mestre; o lobby chama em 0x0801A688 logo apos o 1o `LinkMain`). Sem isso so ocorre 1 troca. Isto explica o filho "preso" em pc 0x08151DB0 do Nasus se o mestre nunca chamar esse ponto (a verificar com traco).
**Cenarios de erro** (`0x02038580`; tamanho de warm-up: os primeiros ~4 quadros dao 3 e depois 6 porque ainda nao ha pacote valido — limpe `0x02038580` e `LS+4 &= 0x7FFF` apos o aquecimento, como o teste faz):
| erro | como forcar (na bancada) | no jogo real |
|---|---|---|
| 3 tipo != 0x20 | gancho `tamper`: `G_SEND[0]=0x21` no filho em 1 quadro; o mestre reporta 3 | enviar pacote de lockstep corrompido / versao de payload diferente |
| 4 sessao | `LS+6` diferente entre as unidades (0x5A x 0x77); reportado por ambos em <1 quadro | sessoes iniciadas separadamente (byte `0x020382D6` diferente) |
| 5 anel estourou | **nao reproduzido**: vem de `0x08030D4C` (procura o quadro N-5 no anel de entradas), chamada so de 0x0801AA04/0x08020140/0x08032AD4/0x08032DE4 (lobby/sessao), nao do `LinkFrameSync`; apagar o anel ou saltar o contador nao dispara | so por atraso/avanco de >= 5 quadros nesses caminhos; precisa de traco real |
| 6 buraco / 0xFFFF | desligar um jogador (`cable.present[i]=False`); mestre reporta 6 no mesmo quadro | cabo solto / jogador sai / rede perde o pacote |
| 7 parado > 16 quadros | igual a 6 MAS com `LS+0x2A0+p = 2` nas duas unidades (profundidade alvo >= 2) — o jogo para ~16 quadros e da 7 | so se algum codigo escrever a profundidade alvo (nao ha referencia literal a 0x02038570-73; ver pendencia) |
| 8 checksum de estado | `check=True` (`LS+4` bit3) e inverter bits 10-11 de uma entrada (`G_SEND[3] ^= 0x0C`); o mestre da 8 em 1 quadro; sem a adulteracao nao ha erro | dessincronia real (RNG/estado diferente) com a flag de verificacao ligada |
**Fases do lobby (decodificacao estatica)**: pacote de lobby = byte0=2, [1]=modo/versao, [2..3]=0x0123, [4..0xB]=perfil de 8 B, [0xC..0xD]=mascara de ack, [0xE]=fase (0x20,0x22,0x40,0x41,0x42), enviado pelos emissores 0x0803169C (hello), 0x08031764/0x08031860/0x080319F0/0x08031AA8. Ordem dinamica e o escritor do bit19 ainda dependem de tracos reais.
**Pendencias**: (a) quem escreve a profundidade alvo `LS+0x2A0+p`; (b) erro 5; (c) tracos reais do lobby: FEITO na secao 13 (via `--peek`, sem mudar o runtime); (d) meta-teste com Nasus: a bancada e o runtime devem produzir o mesmo fluxo de halfwords.


### 13. P0c: caminho de menu ate o co-op, lobby decodificado com tracos reais (Poppy, 2026-10-16)
**Correcao da premissa.** A tela `TRANSMITTING... Please connect the Game Boy Advance Game Link cable` NAO e so o handshake de download play: e uma etapa de **deteccao** (biblioteca em `0x0803024C..0x08030D4C`, tabela em `link_functions.csv`, camada `lobby`) que decide, por console, entre "baixar o jogo" (o filho responde `0x72xx`, caminho multiboot) e "todos tem o cartucho" (o filho responde `0x8F51`; **multi-pak**, `0x0300050C = 1`). Com as duas instancias tendo a ROM e o resultado multi-pak, o jogo liga o **gLink** (`LinkEnable` 0x0815898C) e segue para o lobby de pacotes. Isso vale para **SUB-GAMES > MULTI PLAYER** (`SubGameLobbyCreate` 0x08019F28, `0x0203AD10 = 6`) e para o **co-op da aventura** (`START GAME > MULTI PLAYER`, `AdventureLobbyCreate` 0x08032B0C). O lobby com `LinkStartTimer` (0x0801A688) e o dos sub-jogos; o co-op usa `0x08031DF0`.

**Caminho de menu (titulo -> co-op), medido com `rt_boot.exe` em modo link** (o contador de quadros do `--keys` e o do hw: nas telas de titulo/arquivo o jogo espera DISPSTAT por polling, entao em modo SEM link o contador para; use sempre `--link`):
| quadro | tela | tecla | condicao de espera / observacao |
|---|---|---|---|
| ~600 | titulo "PRESS START" | `A@700+8` | pronto a partir de ~700 |
| ~1000 | FILE SELECT (FILE 1) | `A@800+8` e `A@1060+8` | **a selecao do arquivo so vale com 2 pressoes** (800 e 1060); sem a de 800 o jogo cai em 1 jogador (`0x0203AD10 = 4`). NAO remova nenhuma tecla |
| ~1100-1300 | GAME SELECT (START GAME no topo) | `A@1300+8` | abre o popup ONE PLAYER / MULTI PLAYER |
| ~1330 | popup (cursor em ONE PLAYER) | `DOWN@1350+4`, `A@1400+8` | A sem o DOWN escolhe 1 jogador |
| ~1500 | TRANSMITTING "Please connect..." | — | deteccao: `0x03000490` = {ID, jogadores, estado, flags}; `0x0300050C` = 1 |
| ~1524 | idem, ate "OK?" | — | os dois ficam com `[2]==2` (jogadores=2); o P1 espera ~90 quadros e mostra **OK?** (P2 aparece no filho) |
| ~1700 | "OK?" (so no P1/host) | `A@1700+8` (**so o host**) | confirma; o host manda `0xE4E4`, `[2]=3`, o jogo chama `LinkEnable` (quadro ~1713, `0x03002558 = 1`) |
| 1715-1745 | lobby gLink (secao abaixo) | — | ~30 quadros; termina com 1 quadro de pacote vazio |
| ~1745-2000 | GAME PAK SELECTION ("CHOOSE A GAME PAK TO PLAY", P1/P2) | `A@2000+8` (**os dois**) | escolher o arquivo de cada console |
| 2017-2365 | contagem de inicio (pacotes tipo 3) | — | ~350 quadros |
| 2376-2398 | 2o lobby (mesma sequencia de fases, perfil novo) | — | |
| ~2419 | fase de jogo: `0x0203AD10 = 2`, pacotes de teclas (sessao 1) | — | co-op rodando (Rainbow Route, HUD com telefone rosa no P1 e laranja no P2) |

Scripts (formato `--keys`; tambem em `tools/link_trace.py --coop-keys host|join`):
- **host**: `A@700+8,A@800+8,A@1060+8,A@1300+8,DOWN@1350+4,A@1400+8,A@1700+8,A@2000+8`
- **join**: `A@700+8,A@800+8,A@1060+8,A@1300+8,DOWN@1350+4,A@1400+8,A@2000+8`
Execucao: host = `rt_boot.exe ROM --link host --link-players 2 --link-port P --keys <host> --frames 3001 --sav none --mute ...`; 1 s depois join = `... --link join --link-host 127.0.0.1 --link-port P --keys <join> ...`. **5/5 execucoes identicas** (md5 do quadro 3000 igual; `--peek` igual quadro a quadro em duas execucoes), ~10 s para 3000 quadros. A ordem importa pouco, mas se a mesma tecla for pressionada fora da janela o jogo cai em outro menu (ex.: sem `A@800` -> 1 jogador).

**Como saber que chegou** (`--peek`; valores medidos nos dois processos no quadro 3000):
| nivel | endereco | valor |
|---|---|---|
| deteccao concluida | `0x03000490` = `00020200` (host) / `00020201` (join) e `0x0300050C = 1` | jogadores = 2, estado 2 (3 depois do OK) |
| link ligado | `0x03002558` | **1** (a partir do quadro ~1713) |
| gLink sincronizado | `0x03002554` | host **0x8393**, join **0x8303** (bit 15 = sincronizado, bit 7 = mestre, bits 8-9 = mascara dos 2 jogadores) |
| co-op rodando | `0x0203AD10` = **2**, `0x0203AD30` = **2**, `0x02038580` = 0 (sem erro) | quadro >= ~2420 |
Se `0x0203AD10 = 4` e `0x0203AD30 = 1` o jogo entrou em 1 jogador (tecla fora de lugar). `0x0300050C = -1` com `0x03000490 = 0` = a deteccao ainda nao comecou/terminou.

**Deteccao (referencia em `tools/link_sim.py`)**: os dois consoles trocam `[mestre] 0x6200|contador` / `[filho] 0x8F51` (1 troca por quadro), depois `0x70AE`; no quadro ~40 `[1]=1` e ~70 `[1]=2`; o mestre so passa a `[2]=3` quando o host chama `DetectHostConfirm` (0x08030C1C, tecla A do "OK?") e passa a mandar `0xE4E4`. Reproduzido no Unicorn (2 unidades) com timing ideal e igual ao das duas instancias reais (`0x03000478` = `8F51211D`, `70AE2135`...). Os dois consoles ficam com `0x0300050C = 1` (nao ha "mestre = 1 / filho = 2" aqui).

**Fases do lobby (pacotes `byte0=2`, `[2..3]=0x0123`, bytes 4-11 = perfil, 12-13 = mascara de ack, 14 = fase), do host (P1) e do join (P2), quadros do 1o lobby**:
| quadro | quem | fase `[0xE]` | ack `[0xC..D]` | significado |
|---|---|---|---|---|
| 1715 | host | 0x00 idle | 0000 | 1o pacote apos `LinkEnable` (perfil `0000ff03 08141EB3`) |
| 1718 | host | **0x20 hello** | 0001 | ja ouviu a si mesmo (bit 0) |
| 1720 | join | **0x40 child-hello** | 0003 | ouviu o hello do host; ack dos jogadores 0 e 1 |
| 1724 | host | 0x20 | **0003** | ouviu o filho (bits 0,1) |
| 1727 | join | **0x41 child-ack** | 0003 | status passa a 0x8303/0x8393 (bit 15) |
| 1735 | host | **0x22 host-ready** | **0013** | bit 4 = confirmacao do P1 |
| 1737-1740 | join | **0x42 child-ready** (alterna com 0x41) | **0002** | so o proprio bit |
| 1741 | host | 0x22 | **0033** | bits 4 e 5 = confirmacao dos dois: lobby completo |
| 1744 | todos | pacote vazio (1 quadro) | | fim do lobby |
| 1745 | host | **tipo 0x20** (teclas), sessao **02**, quadro 0 | | o lockstep comeca; join no quadro 1744 |
Leitura: ack `0x0F` = jogadores de quem o hello foi ouvido; `0x10<<p` = confirmacao do jogador p (so o host as agrega: 0x13 -> 0x33). Fases 0x20/0x22 = host; 0x40/0x41/0x42 = filho; as trocas 0x41 <-> 0x42 do filho sao reenvios ate o 0x22 chegar. Depois do GAME PAK: pacotes **tipo 0x03** (`03 02 CC 00..`: byte1 = sessao 02, byte2 = contador que sobe 1 a cada 6 quadros de 0 a 0x35; no 0x34 o host manda parametros extras `[8]=02, [0xF]=FF`), e o **2o lobby** (quadros 2376-2398, mesmas fases, perfil `92edfb5c 00001408` = semente/ID da fase) seguido de pacotes de teclas com sessao **01** a partir do quadro ~2419 (`0x0203AD10 = 2`).

**Flag bit19 de `0x03002440`**: **nunca vale 1** neste caminho (amostrado a cada quadro de 1660 a 2555 nos dois processos: so oscilam os bits 0-1 a cada 13 quadros; bit 24 fixo). Funcao: `LinkEnable`/`LinkDisable` e o laco principal `0x08151C54` so a leem para decidir se refazem `LinkInit(0)`/o vetor de IRQ (`0x08158F0C`); o link funciona sem ela (`LinkEnable` apenas escreve `0x03002558 = 1`). Quem a liga e outro caminho (candidatos: 0x0801CC30, 0x0803CD98/0x0803CFC4, 0x0803E050, 0x08083494, 0x08124430) que este fluxo nao executa. **Nao e pre-requisito para o co-op.** (No `link_sim` eu a ligava so para exercitar o `LinkInit`.)

**Decodificador**: `tools/link_trace.py` le o log do `--peek` (`python tools/link_trace.py host.log join.log`) e imprime so as mudancas (fases do lobby, tipo 0x20, tipo 3, status); `--peek-arg` da a lista de 30 enderecos; limite do runtime: `--snap` com <= 128 quadros por execucao. Teste: `tests.test_link_protocol` (30 testes, ~20 s) inclui decodificador offline e **duas instancias reais ate o co-op** (`TestTwoInstancesCoop`, usa uma porta livre em 127.0.0.1; logs em arquivo, nao pipe: com pipe os dois processos travam por buffer cheio).

**Pendencias antigas**: (a) **profundidade alvo `LS+0x2A0+p` e o erro 5 tem a mesma origem**: `0x08030D4C` (chamada so de 0x0801AA04, 0x08020140, 0x08032AD4, 0x08032DE4 — lobby/sessao, nunca do `LinkFrameSync`) procura no anel de entradas do jogador p a entrada de quadro `LS.frame - 5`; se nao acha -> erro 5; se acha grava `(cabeca - posicao) & 0xF` em `LS+0x2A0+p`, e e esse valor >= 2 que liga a espera do `LinkFrameSync` (erro 7). Em jogo normal ninguem chama `0x08030D4C`, por isso a profundidade fica 0 e a queda de jogador da erro 6. (b) Nao reproduzi o erro 5 em execucao: precisa chegar a um dos 4 chamadores (pausa/retomada da sessao). (c) A flag `0x2B` de `0x03000490` e o unico "OK?" do host: sem a tecla A do host o co-op nunca comeca (o join sozinho nao basta).
