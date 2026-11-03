# Evolução do port para Nintendo Switch

Linha do tempo do que foi feito, medido e decidido (tudo em 2026-11-02, builds b1 a b64). Mantida **a cada rodada**: depois de cada teste
entra aqui o que o log/relatório mostrou. O que falta fazer fica em `DEBITO-TECNICO.md`.

Como ler os números: `fps` e `gcpu` (ms que a thread do jogo gasta por frame) vêm de `log.txt`/`relatorio.txt` do console;
"PC" = Ryzen 5900X; "yuzu" = emulador em `F:\yuzu` (mesmo código arm64, **não** tem a velocidade do console).

## Marcos

| Fase | Builds | O que passou a existir | Medida / resultado |
|---|---|---|---|
| Cliente de rede | (antes) | `homebrew/switch-net` (n2n): cliente C do `/lan` do servidor, TLS com mbedTLS | Testado contra servidor falso e contra o `nfsu2.xyz` real |
| Levantamento | - | Código do jogo traduzido para C (`web/gen.sh`) compila para aarch64 (130 shards, 25,8 MB de texto); runtime com `NFSU2_HOST_PLATFORM`; 56 de 67 fontes do runtime compilam sem mudança | Risco principal: CPU do A57 |
| Benchmark | b1-b3 | NRO sem vídeo (`null_platform.c`), entrada `nx_main.c`, host `nx_host.c` (futex por `svcWaitForAddress`, `pread`) | Menus 1,3-2,9 ms/frame; fase de boot pesada 195-305 ms/frame; Linux x86 no PC é 10-15x mais rápido |
| Primeiro hardware | b2-b3 | Tomada de título (R + abrir um jogo): modo applet só dá 403 MB, o jogo precisa de 640 MB | Corrigido com título tomado: 3189 MB |
| Arquivos estáveis | b5-b10 | Camada própria de leitura do SD (`fsFileRead`, fds sintéticos a partir de 2000, buffer de 1 MB), `/dev/null` trocado por arquivo vazio, checagens no hook do printf, `crash.txt` | Fim das quedas em `pread`/`_lseek_r` |
| "Slideshow" | b4-b9 | Rasterizador por software (`gpu_native.c`) lendo os registros do `gpu_web.c`; thread própria; escala 1/2 (640x360) | Menu 56-60 fps; legível |
| Menus rápidos | b7 | `NFSU2_SKIP3D=1`: descarta desenhos com teste Z antes de qualquer vértice | Menu principal 5 -> 50-60 fps |
| Renderer GL | b11-b16 | OpenGL 4.3 core (Mesa 20.1 / NV120): `wgsl2glsl.c` traduz os 83 shaders do jogo, `gpu_gl.c` porta o `encoder.ts` (vertex pulling por SSBO, UBO de 64 KB, FBO, blits) | 0 falhas de shader; corrida roda, menu 60 fps; congelamento do b13 não repetiu |
| Janela | b15 | `nwindowSetDimensions(1280x720)`; saída ordenada (`gpu_native_stop`, `glr_shutdown`) | Imagem visível, `+` sai limpo |
| Pista carregando | b16 | Corsa/Peugeot 106 só entram na lista se os arquivos existirem (`regional.c`) | Corrida carrega (era loop eterno em PC_Loading) |
| Boost de CPU | b17-b22 | `cpu_boost()` a 1785 MHz | **Piorou** (16-21 fps contra 38-44). Desligado por padrão; só liga com o arquivo `boost`. Causa não confirmada |
| Perfil de desenho | b18-b21 | Contadores `record`/`swap`/`prof(...)` no `[gpu-nx]` | O laço de desenho é ~3 ms; o resto era o boost. Lição: nunca ler registradores ARM por asm no Horizon (b20 caiu) |
| Ícone, versão | b23-b24 | `BUILD` incrementa por `build.sh nro`; versão `bN` no NACP; ícone com o logo do jogo | Aprovado |
| Gráficos | b25-b40 | Preset gravado em `.nfsu2_registry` (FAT não troca arquivo por `rename`: `unlink` antes), `NFSRES` fixa 720p | Preset N (sem rastros, tingimento, plateia): 1 passe/frame, ~300 draws, mas o fps não voltou ao do b22 |
| Controle | b26 | `pad_to_game()` com o layout nativo do jogo (gatilhos analógicos digitais, sticks, Minus+Plus 1 s sai) | "Quase ok" |
| Pacote único | b35-b37 | `nfsu2.dat` (1256 arquivos, 1,7 GB hoje 1274/2,1 GB) com índice e CRC, `vfs_dat.c` (stat/opendir/fopen virtuais), áudio (`audio_nx.c` -> audout 48 kHz) e teclado do console (`text_nx.c`) | Som ok, nome de perfil ok; `relatorio.txt` por fase |
| Saída | b39-b40 | `lifecycle_request_quit` + espera de 15 s, depois `return` de `main` | Sai sem erro pelo menu do jogo |
| Contador de fps | b41 | Texto 5x7 desenhado na CPU numa textura 288x30, L3+R3 liga/desliga | Build "seguro" por muito tempo |
| Madrugada | b42-b45 | Fixar threads por núcleo (sem efeito no fps), cache de binários de shader (**impossível**: o driver tem 0 formatos), vibração HID, **camada online** (`lan_nx.c`: thread de rede com os anéis NET e LAN, 118 certificados Mozilla embutidos) | Online chega até o menu; vibração funciona |
| Online jogável | b46-b58 | Identidade pela conta do Switch (hash + apelido), `endereco.txt` retoma a sala, carros-fantasma (`COLLIDE=0`), taxa 15 Hz, 4 -> 2 carros | LAN com jogadores funciona. No free roam **o hub devolve listas com 0 jogadores** (ver abaixo) |
| Dois consoles | b51 | Segundo Switch falhava por relógio em 2025 (certificado "ainda não válido"): corrigido no console; o b51 tolera data futura do certificado | Os dois consoles se acham |
| Custo dos amigos | b58-b61 | `relatorio.txt` por número de amigos | 0 amigos ~39-40 fps; 1 carro 25-29; 4 carros ~20. **O primeiro carro remoto custa ~14 ms** |
| Resolução | b62-b64 | `resolucao.txt` (LxA, até 1280x720) fixa `NFSRES`; GL estica para a janela 1280x720; 960x540 deu 29-30 fps (contra 21 em 720p) | Ver "b64" abaixo |
| Banco de testes | b63-b64 | yuzu 1734 em `F:\yuzu`, SD virtual em `%APPDATA%\yuzu\sdmc\switch\nfsu2`; marcadores `captura`, `software`, `lento.txt`, `show.txt` (roteiro automático), `save_bench` vazio com roteiro; log rotativo (`log1.txt`, `log2.txt`) | `nx_career_roam.txt` leva do título ao free roam em ~100 s no yuzu |

## Decisões que valem lembrar
- O repositório é a sandbox: nada fora dele; nada gravado no SD sem o usuário pedir. `/switch/nfsu2x` não é nosso.
- Só **um** executável no SD (`nfsu2_bNN.nro` com o número no nome; cópia por MTP **não sobrescreve** nome existente; só nome novo na listagem prova que chegou).
- Menos "uma build, um teste": agrupar em candidatas a release, validar o que dá no PC/yuzu antes e fazer o console escrever um relatório.
- **Usar o yuzu antes de pedir teste no OLED**; o console só entra para medir velocidade.
- Hub do servidor: nome reivindicado na web (ex.: o do usuário) faz o Switch virar "impostor" e não receber listas; o Switch joga como apelido+4 dígitos.
- Vibração do console V1 não funciona (o jogo toca os efeitos, HID retorna ok): limitação da build do PC, abandonada.

## Rodada b64 (OLED, 87 s, free roam, 960x540, opções do jogador, 2 carros)

Dados de `log.txt` e `relatorio.txt` lidos do SD pelo MTP (somente leitura).

| Medida | Valor |
|---|---|
| fps menus | 41,7 médio (mínimo 2 num carregamento) |
| fps corrida | 20,5 médio; 2 amigos 21,3; 3 amigos 20,4; 4 amigos 20,0 (mínimo 14-16) |
| `gcpu` da thread do jogo na corrida | 33-37 ms/frame (CPU em 84%: sobra ~16% de espera) |
| Renderer na corrida | `record` 15,6-20,9 ms (laço `draw` 6,5-8 ms, `bufbind` 0,5-0,9, `tex` ~1), `swap` 6-6,4 ms, 1 passe/frame, ~620 draws/frame |
| GIL | 1322 de 2269 aquisições na fila (58%) |
| Leitura do `.dat` na corrida | 468 leituras, 25 MB, 3,3 s no total (7 ms cada, maior 47 ms) |
| Rede | 2914 quadros entrando, 2583 saindo, 0 perdidos; 1 conexão |
| Áudio | 4041 buffers, 7 underruns |
| Gráficos | `"the player's options (the preset was written before)"`: os valores eram os do jogador, não o preset |

Leitura: com 960x540 e 2 carros a corrida fica em ~20 fps, **pior** que o 29-30 do b62 com a mesma resolução. A diferença provável é
que o b64 rodou com as opções do jogador (mais passes e efeitos: `record` subiu para 15-21 ms, contra 5,5 no b22) e não com o preset. O custo da
thread do jogo (33-37 ms) é o limite, e `swap` de 6 ms indica que a GPU também começou a pesar. O número de amigos quase não muda o fps
quando já há ao menos 2 (o primeiro carro remoto é o caro).

## Pendências abertas da rodada
1. O que o primeiro carro remoto dispara (carregamento do modelo? hooks de física? ~14 ms) -> perfilar no yuzu com `lento.txt` calibrado.
2. Calibrar `lento.txt` com este log (console: menu ~7-8 ms/frame, race 33-37 ms; yuzu: menu ~0,7).
3. Free roam sem jogadores: o PC vê 16 no hub, o Switch vê 0 (suspeita no filtro do hub por campos do HELLO/SNAP).
4. Preset de gráficos não é respeitado em todos os campos (ver DEBITO-TECNICO.md seção 2).

## Calibração do yuzu (b65, 2026-11-02, sem `software`, `captura` nem `lento.txt`)
Roteiros em `tests/scripts/`: `nx_career_roam.txt` (título -> perfil -> menu principal -> carreira: no b65 só chegou ao menu, 4 ms/frame
contra ~10 ms no console) e **`nx_quick_race.txt`** (novo: perfil -> Quick Race -> 1500 frames dirigindo -> sai). Copie o roteiro para
`%APPDATA%\yuzu\sdmc\switch\nfsu2\show.txt` e rode `runtime\switch\yuzu-run.ps1 -Seconds 150 -Nro out\switch\nfsu2.nro`.

| | yuzu (PC, sem amigos) | console OLED |
|---|---|---|
| Menu principal (`gcpu`) | ~4 ms | ~10,5 ms (b64) -> fator ~2,6 |
| Corrida (`gcpu`) | 14-19 ms depois de carregar (25 no início) | 19-22 ms sem amigos (b22); 33-37 com 2-4 amigos (b64) -> fator ~1,3-1,5 sem amigos |
| fps da corrida | 45-57 | ~39-48 sem amigos |

Conclusão: o yuzu é um **bom substituto de custo relativo** para a thread do jogo (≈ console sem amigos), então mudanças que reduzam `gcpu`
no yuzu devem reduzir no console. Não substitui o teste de GPU/driver (Mesa/nouveau do console x driver do PC): `record` no yuzu 8-10 ms (b64
console 16-21 ms). Para medir o custo do carro remoto no yuzu faltam amigos: ligar `online` (conecta ao hub real, visível ao servidor) ou usar o
servidor local de teste (`roam-pair`).

## b66: yuzu deixa de ficar preto e o teclado não trava mais (2026-11-02)
- **Tela preta no yuzu (causa achada):** o log do yuzu (`%APPDATA%\yuzu\log\yuzu_log.txt`) dizia `Translate LD: Instruction LD is not implemented`
  uma vez por pipeline. O Mesa do app compila a leitura de vértices por SSBO (`readonly buffer VB`) em `LD` do Maxwell, e o recompilador de shaders do
  yuzu não implementa essa instrução: o pipeline falhava, as chamadas de desenho iam, e a tela só tinha o contador de fps.
- **Correção:** `NFSU2_NX_TBUF=1` (arquivo vazio `tbuf` na pasta do app): `wgsl2glsl.c` emite o vetor `VB` como `usamplerBuffer` (unidade de textura 12) e as
  leituras viram `texelFetch(VB_tb, i).x`; `gpu_gl.c` liga o buffer com `glTexBuffer(GL_R32UI)` junto do bind do SSBO. Verificado: os 83 shaders traduzem e compilam no
  llvmpipe com a flag; no yuzu, erros `LD` 11 -> 0, 13 programas, 0 falhas de shader, imagem correta (menu de pista do Quick Race, perfil CLAUDE). **Fica desligado no
  console** (sem o arquivo); pode ser um teste de desempenho futuro (leitura de textura em vez de SSBO).
- **Teclado do sistema travava a criação do perfil:** com `show.txt` (roteiro de teste) o app não abre mais o `swkbd` (o applet esperaria um humano); o roteiro digita
  (`[nx] test: script mode, no system keyboard`). Em uso normal nada muda.
- Como rodar sem tocar no console: pasta `%APPDATA%\yuzu\sdmc\switch\nfsu2\` com `tbuf`, `captura` (salva `captura.png`) e `show.txt` (copie
  `tests/scripts/nx_quick_race.txt`); `powershell runtime\switch\yuzu-run.ps1 -Seconds 100 -Nro out\switch\nfsu2.nro`; leia `log.txt` e `captura.png`. Não usar `software`
  no yuzu: o renderer por CPU não é o que roda no console.
- Console: a última execução no SD ainda é a do b64 (log de 75569 bytes, lido duas vezes: nenhuma execução nova).

## yuzu configurado para ficar próximo do console (2026-11-02)
`%APPDATA%\yuzu\config\qt-config.ini` (backup: `qt-config.ini.bak-before-fidelity`): backend **OpenGL** mantido (o Vulkan usa o mesmo recompilador de shaders: não evita o `tbuf`), `memory_layout_mode=0` (4 GB: o app agora vê ~3285 MB, o console dá 3189 MB; antes 6547 MB), `use_docked_mode=0` (portátil, como o OLED), otimizações CPU "unsafe" desligadas, shaders assíncronos desligados, GPU Normal, resolução 1x, limite de fps 100%.
O yuzu continua sem a velocidade do console (menu ~1 ms contra ~7-10 no console; corrida 18-30 ms).

## Rodada de otimização (b67-b70, 2026-11-02): nada atingiu 20%, nada foi copiado ao console
- **Roteiro lento corrigido:** o passo do nome do perfil esperava 3000 frames (~50 s) porque, depois do "Yes", aparece uma tela de continuar que o roteiro não pressionava. `nx_quick_race.txt` agora é orientado a eventos (`tapuntil RETURN GarageMain.fng`): menu principal no frame 1015 (era 4548), uma rodada de `yuzu-bench.ps1` leva ~90 s (era ~150 s). `yuzu-run.ps1` termina quando o app sai.
- **Linha de base no yuzu** (b67, 4 GB, portátil): corrida `gcpu` 17,1-17,8 ms/f, 43-48 fps; ruído entre rodadas ~0,2 ms (`out/yuzu-bench.csv`).
- **Experimento 1, `NXGCONST=1` (base da memória do guest como `const` nos shards):** NRO 5% menor, ganho **0%** (17,65 contra 17,7 ms/f). Opt-in, padrão desligado, não vale levar ao console.
- **Perfil `perf` (harness Linux/WSL, corrida do mesmo roteiro, thread do jogo):** distribuição plana. 55% das amostras em código traduzido (`sub_005cf420` 2,6%, `sub_0066055c` 2,3%, `sub_005c5930` 2,0%, `sub_005d6570`, `sub_00620ae0`... nenhum acima de 3%), `frustum_native` 3,6%, `com_call` 2,8%, `x87_r2pc` 1,7%, `vs_run` 1,3% (vértices por CPU). Consultas de relógio (`nfsu2_now_ms` + vdso) somam ~17% **no PC**, vindas do GIL (vários carimbos por troca) e do `QueryPerformanceCounter`/`timeGetTime` do jogo (~1000 por frame). No console isso custa menos: `clock_gettime` da libnx é só `mrs cntpct_el0` mais aritmética (verificado no `newlib.o`); no yuzu custa mais (callback do JIT), então o yuzu superestima este item.
- **Conclusão:** não há um ponto quente único na thread do jogo. Os ganhos prováveis estão em (a) o custo do primeiro carro remoto (~14 ms, ainda sem causa), (b) reduzir as trocas do GIL e seus carimbos, (c) menos chamadas de relógio por frame, (d) `record` do GL (16-21 ms no console contra 8-10 no yuzu). Para (a) falta o servidor local de amigos.

## Continuação da rodada (b71-b72, 2026-11-02): GLOCAL e -O3 também 0%
- **`NXGLOCAL=1` (b71):** `LR_ENTER` copia `g_mem` para `uint8_t *restrict _gm` e os shards usam isso (`cpu.h`). No gcc x86-64 de `sub_00583100` (cluster quente do `perf` com o raster por software): cargas RIP de `g_mem` **16 → 1**. Mesmo assim: harness Linux `gcpu` médio na corrida 3,04 → 3,08 ms (0%); yuzu 17,94 e 18,57 ms/f. Leitura: o tempo está no tráfego guest, não no reload do ponteiro-base. `NXGCONST` já fazia o mesmo no asm x86; no yuzu/aarch64 também não pagou.
- **`NXO3=1` (b72):** shards em `-O3` (runtime em `-O2`). yuzu 18,48 e 17,87 ms/f; NRO 37,0 MB (base ~34). Sem ganho.
- **Nada copiado ao console** (regra dos ~20%). Linha de base yuzu continua ~17,7 ms / ~43 fps (`out/yuzu-bench.csv`).
- Scripts de apoio: `runtime/switch/perf-race.sh`, `perf-resolve2.py`, `perf-gconst-cmp.sh`.
