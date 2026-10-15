# Kirby & The Amazing Mirror (GBA) - recompilação estática

[English](README.md) · **Português**

Projeto de estudo: transformar o código do jogo (ARM7TDMI, a CPU do GBA) em C e rodá-lo
nativamente no PC, sem emulador. **Não é uma decompilação** que reconstrói o código-fonte
original: o recompilador traduz as instruções da ROM uma a uma e liga o resultado a um
runtime que imita o hardware (vídeo, DMA, timers, interrupções, BIOS).

> **Nenhuma ROM, BIOS ou material do jogo está neste repositório.** Você precisa da sua
> própria ROM (USA, `B8KE`), colocada como `baserom.gba` na raiz. Ela é ignorada pelo git,
> assim como tudo o que é gerado em `build/`.

## Estado

| Etapa | Situação |
|---|---|
| Análise da ROM (código × dados, ARM × Thumb, funções) | feito: 5.669 funções mapeadas, mapa congelado e com teste de regressão |
| Recompilador ARM/Thumb → C | funciona: o jogo inicia e roda mais de 8.000 quadros sem abortar |
| Hardware | ligado: fundos tiled/afins, sprites, janelas, blending, DMA, timers, áudio PCM |
| Imagem | título, abertura e a primeira fase aparecem corretos (salvos como imagem) |
| Janela e teclado | funciona: `python -m recomp.program --window 3` (Z/X = A/B, Enter = Start, setas) |
| Saída de som | o áudio PCM e PSG é gerado e existe uma saída SDL2, mas ela ainda não está ligada ao executável do jogo |
| Save | funciona: `baserom.sav` de 32 KiB |

Mais detalhe, com números e decisões, em [`NOTES.md`](NOTES.md) (resumo em inglês: [`NOTES.en.md`](NOTES.en.md)).

## Como funciona

```
baserom.gba ──► análise ──► mapa de funções ──► recompilador ──► C ──► programa nativo
                                                                         ▲
                                                       runtime (memória, BIOS, hardware)
```

1. **Análise** (`tools/`): lê a ROM, segue o fluxo a partir do ponto de entrada e decide o que
   é código ARM, código Thumb, tabela ou dado. O resultado é um mapa de funções com um nível
   de confiança para cada uma.
2. **Recompilador** (`recomp/`): cada instrução vira poucas linhas de C sobre um estado de CPU
   (`r0`–`r15`, flags). Chamadas por ponteiro de função passam por uma tabela em tempo de
   execução, porque o alvo só é conhecido rodando.
3. **Runtime e hardware** (`recomp/runtime/`, `hw/`): barramento de memória, as chamadas da BIOS
   que o jogo usa (cerca de 13) e o modelo dos registradores de vídeo, DMA e timers.

## Estrutura

| Pasta | Conteúdo |
|---|---|
| [`tools/`](tools) | análise da ROM, saltos indiretos, pontuação de confiança |
| [`recomp/`](recomp) | decoder, gerador de C e runtime |
| [`hw/`](hw) | modelo do hardware do GBA e seus testes |
| [`tests/`](tests) | testes do recompilador (comparação instrução a instrução com o Unicorn) |
| [`NOTES.md`](NOTES.md) | notas técnicas, decisões e resultados |
| [`ROLES.md`](ROLES.md) | quem cuida de quê, no trabalho em paralelo |

## Início rápido (sem Python)

Monte a pasta de lançamento uma vez com `python tools/package_release.py --zip` (cerca de 95 MB
compactada) ou baixe uma versão quando houver. Depois:

1. Tenha a sua ROM (USA, código `B8KE`).
2. **Arraste a ROM sobre `KirbyRecomp.exe`.**
3. Na primeira vez ele constrói o jogo no seu computador (uns 5 minutos, com janela de progresso).
   Nas próximas abre em menos de um segundo.

O jogo gerado fica em `%LOCALAPPDATA%\KirbyRecomp`; o pacote não contém nada do jogo.
Controles: X = A, Z = B, Enter = Start, Backspace = Select, setas, A/S = L/R, F1 = launcher, F11 = tela cheia, Esc = sair.
O Windows pode avisar sobre programa não assinado: o launcher executa um compilador e o programa que ele gera.

## Como rodar a partir do código-fonte

Requisitos (Windows): Python 3.13 e um compilador C (GCC via WinLibs, ou `zig cc`).

```
pip install capstone

python tools/analyze_rom.py                  # gera build/analysis/ (mapa de funções)
python tools/resolve_indirect.py baserom.gba # classifica saltos indiretos
python tools/score_functions.py baserom.gba  # confiança de cada função
```

O recompilador e seus testes usam um ambiente próprio (`unicorn`, `ziglang`); os comandos
completos estão em [`NOTES.md`](NOTES.md), seção "Recompilador mínimo".

## Aprendizados até aqui

- O jogo despacha quase tudo por tabelas de ponteiros, então seguir só as chamadas diretas
  acha menos de 10% das funções.
- Dos ~5 mil `bx rN`, quase todos são retornos de função (`pop {r0}; bx r0`), não saltos.
- O compilador original usa `bl` como salto longo para o epílogo de funções grandes. Se isso
  for tratado como chamada, o recompilador gera código errado.

## Projetos que serviram de referência

[N64Recomp](https://github.com/N64Recomp/N64Recomp),
[gba-recomp](https://github.com/JRickey/gba-recomp),
[gbarecomp](https://github.com/mstan/gbarecomp). Detalhes e licenças em `NOTES.md`.

## Licença

O código e a documentação deste projeto estão sob a [licença MIT](LICENSE). Isso cobre apenas este
projeto: *Kirby & The Amazing Mirror* pertence à Nintendo e à HAL Laboratory e não está incluído.

## Aviso legal

Projeto não oficial e sem fins comerciais, sem vínculo com a Nintendo, a HAL Laboratory ou
os demais detentores dos direitos de *Kirby & The Amazing Mirror*. O repositório contém
apenas ferramentas e código próprio; o jogo é do usuário.
