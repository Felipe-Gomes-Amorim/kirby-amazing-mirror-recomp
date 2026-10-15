# Kirby & The Amazing Mirror (GBA) — recompilação estática

[English](README.md) · **Português**

## Sobre

Projeto de estudo: transformar o código do jogo (ARM7TDMI, a CPU do GBA) em C e rodá-lo
nativamente no PC, sem emulador. O recompilador traduz as instruções da ROM uma a uma e liga o
resultado a um runtime que imita o hardware (vídeo, DMA, timers, interrupções, BIOS).

**Não é uma decompilação** (não reconstrói o código-fonte original) **e não é um emulador**: o jogo
é traduzido uma vez, antes de rodar.

> **Nenhuma ROM, BIOS ou material do jogo está neste repositório nem nas versões publicadas.** Você
> precisa da sua própria ROM (USA, código `B8KE`). O jogo é traduzido *no seu computador*, a partir
> da sua ROM; tudo o que é gerado fica em `%LOCALAPPDATA%\KirbyRecomp`.

## Início rápido (sem Python)

Baixe o `KirbyRecomp.zip` na página de [Releases](../../releases) (cerca de 95 MB), extraia
em uma pasta onde você possa gravar e:

1. **Arraste a sua ROM sobre `KirbyRecomp.exe`** (ou abra o programa e escolha o arquivo).
2. O launcher abre. Escolha tamanho da janela, tela cheia, escala e volume e aperte **Play**.
3. Na primeira vez o jogo já é jogável em segundos (interpretador embutido) enquanto a versão
   compilada é construída em segundo plano, em alguns minutos e com prioridade baixa. Nas próximas
   aberturas usa o jogo compilado e abre em menos de um segundo.

Controles (todos configuráveis no launcher: **Keyboard...** / **Controller...**):

| GBA | Teclado | Controle (por posição) |
|---|---|---|
| A / B | X / Z | botão leste / sul |
| L / R | A / S | ombros |
| Start / Select | Enter / Backspace | Start / Back |
| Direcional | setas | direcional ou analógico esquerdo |

`F1` volta ao launcher, `F11` alterna tela cheia e `Esc` sai (essas três são reservadas).
O Windows pode avisar sobre programa não assinado: o launcher executa um compilador e o programa que ele gera.

## Estado

| Área | Situação |
|---|---|
| Análise da ROM (código × dados, ARM × Thumb, funções) | feita: 5.675 funções mapeadas, congelado e com teste de regressão |
| Recompilador ARM/Thumb → C | funciona: o jogo inicia e roda mais de 8.000 quadros sem parar |
| Hardware | fundos tiled/afins, sprites, janelas, blending, mosaico, DMA, timers, áudio PCM e PSG |
| Imagem | título, abertura e a primeira fase aparecem corretos |
| Launcher | estado da construção, janela 1x–6x, tela cheia, escala Integer / Fit / Stretch, volume, Play |
| Entrada | teclado e um controle (testado com um genérico), telas de remapeamento, analógico com zona morta ajustável |
| Som | PCM e PSG por SDL2 (ainda não comparado com uma referência) |
| Save | `<rom>.sav` de 32 KiB ao lado da ROM |
| Jogar enquanto compila | interpretador agora, jogo compilado na próxima abertura |

### Limitações conhecidas

- Os testes se concentram na **primeira área**; fases profundas, chefes e todos os poderes não foram
  verificados. Reporte falhas com `%LOCALAPPDATA%\KirbyRecomp\launcher.log`.
- O suporte a controle foi **testado com um controle genérico** e funciona; outros modelos (Xbox,
  PlayStation, Switch Pro) devem funcionar pelos mapeamentos do SDL, mas não foram testados.
- Só a ROM USA. Multiplayer por cabo não está implementado.
- O tempo é aproximado (não é preciso por ciclo); o executável não é assinado.

Números, decisões e problemas abertos: um resumo em inglês em [`NOTES.en.md`](NOTES.en.md) e o
diário completo em [`NOTES.md`](NOTES.md) (português).

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
   execução, porque o alvo só é conhecido rodando. Um interpretador cuida do código que só
   existe na RAM em tempo de execução.
3. **Runtime e hardware** (`recomp/runtime/`, `hw/`): barramento de memória, as chamadas da BIOS
   que o jogo usa (cerca de 13) e o modelo de vídeo, DMA, timers, som e entrada.
4. **Launcher** (`launcher/`, `hw/launcher_ui.c`): tela de configuração, estado da construção e o
   fluxo de jogar enquanto compila.

### Por que isso é mais difícil do que parece

- O jogo despacha quase tudo por tabelas de ponteiros, então seguir só as chamadas diretas
  acha menos de 10% das funções.
- Quase todos os `bx rN` são retornos de função (`pop {r0}; bx r0`), não saltos.
- O compilador original usa `bl` como salto longo para o epílogo de funções grandes. Se isso
  for tratado como chamada, o recompilador gera código errado.
- Parte do código é copiada da ROM para a RAM na inicialização (o mixer de som, rotinas de SRAM)
  e precisa ser reconhecida ou interpretada a partir da imagem na RAM.

## Estrutura

| Pasta | Conteúdo |
|---|---|
| [`tools/`](tools) | análise da ROM, saltos indiretos, confiança, `build_all.py`, empacotamento, testes de estresse |
| [`recomp/`](recomp) | decoder, gerador de C, orquestração do build e runtime |
| [`hw/`](hw) | modelo do hardware, UI do launcher, leitor de configuração e seus testes |
| [`launcher/`](launcher) | fluxo do launcher (Python), stub do Windows, ponte com a UI |
| [`tests/`](tests) | testes do recompilador (contra o Unicorn) e suítes de QA do launcher e das entradas |
| [`NOTES.en.md`](NOTES.en.md), [`NOTES.md`](NOTES.md) | resumo técnico (inglês) e diário completo (português) |
| [`ROLES.md`](ROLES.md) | quem cuida de quê, no trabalho em paralelo |

## Como rodar a partir do código-fonte

Requisitos (Windows): Python 3.13 com `capstone`, um compilador C (`zig cc` pelo pacote `ziglang`,
ou GCC da WinLibs) e os arquivos de desenvolvimento do SDL2 para MinGW. Coloque a sua ROM em
`baserom.gba`.

```
python tools/build_all.py baserom.gba --play 3     # ROM -> análise -> compila -> abre o jogo
python tools/package_release.py --zip              # gera dist/KirbyRecomp.zip
python -m unittest tests.test_launcher_flow tests.test_launcher_qa
```

O `build_all.py` roda as passadas de análise e o `python -m recomp.program`, que compila o jogo e
liga o `kirby.exe`. Os testes do próprio recompilador usam o `unicorn`; os comandos completos estão
em [`NOTES.md`](NOTES.md), seção "Recompilador mínimo".

## Projetos que serviram de referência

[N64Recomp](https://github.com/N64Recomp/N64Recomp),
[gba-recomp](https://github.com/JRickey/gba-recomp),
[gbarecomp](https://github.com/mstan/gbarecomp). Detalhes e licenças em `NOTES.md`.

## Licença

O código e a documentação deste projeto estão sob a [licença MIT](LICENSE). Isso cobre apenas este
projeto: *Kirby & The Amazing Mirror* pertence à Nintendo e à HAL Laboratory e não está incluído.
Os componentes de terceiros do pacote estão listados em `THIRD_PARTY_NOTICES.txt`.

## Aviso legal

Projeto não oficial e sem fins comerciais, sem vínculo com a Nintendo, a HAL Laboratory ou
os demais detentores dos direitos de *Kirby & The Amazing Mirror*. O repositório contém
apenas ferramentas e código próprio; o jogo é do usuário.
