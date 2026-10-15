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
