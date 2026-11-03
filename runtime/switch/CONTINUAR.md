# CONTINUAR.md: passagem de bastão para outra IA (port NFSU2 para Nintendo Switch)

Leia este arquivo primeiro, depois `EVOLUCAO.md` (linha do tempo e medidas) e `DEBITO-TECNICO.md` (o que falta). Atualizado em 2026-11-02, build **b72** em `out/` (última candidata medida; **não** copiar ao console).
Quem continuar deve **atualizar os três arquivos a cada rodada** (regra do usuário).

## Objetivo
App nativo (homebrew) do NFSU2 para Switch OLED que joga online em https://nfsu2.xyz. Hoje: jogo completo roda, renderer OpenGL 4.3 (Mesa/NV120), áudio, teclado do sistema,
controle, `nfsu2.dat` único, online (LAN e free roam) funcionando. Problema aberto principal: **desempenho com amigos na corrida** (~20 fps; sem amigos ~39-48).

## Regras do usuário (valem sempre)
1. **O repositório é a sandbox.** Nada fora dele, exceto: pastas do yuzu (`F:\yuzu`, `%APPDATA%\yuzu\...`, liberadas) e o scratchpad da sessão. **Nada de commit** sem pedido (nada foi commitado desde o b57).
2. **Yuzu primeiro.** Nunca peça um teste no OLED se o yuzu cobre (função, menus, arquivos, rede, travadas, custo relativo da thread do jogo). O OLED só entra para medir velocidade final (GPU/driver/CPU reais). O usuário detesta "uma build = um teste": agrupe correções, valide no PC/yuzu e deixe o app escrever um relatório.
3. **SD do console (via DBI/MTP, só enquanto o usuário o deixa conectado):** ler `log.txt`/`relatorio.txt`/`crash.txt` (somente leitura, copie para o scratchpad) é livre. **Escrever no SD só com autorização.** Autorização vigente (2026-11-02): *"se conseguirmos ~20% de melhora no yuzu, aplique ao console conectado via DBI"*. Sem esse ganho medido, não copie.
   - Nunca toque em `/switch/nfsu2x` (é do usuário). Só **um** `.nro` da nossa app no SD; nome novo por build (`nfsu2_bNN.nro`); cópia por MTP **não sobrescreve** nome existente e **apagar por MTP trava**: depois de copiar, confirme na listagem e diga ao usuário quais `.nro` velhos apagar no Explorer. Nunca diga "está no SD" sem ver na listagem.
4. Docker Desktop e WSL: usar para compilar, e o usuário pediu antes que fiquem fechados quando não em uso (hoje estão em uso).
5. Idioma com o usuário: português. Documentos em português. Pronomes neutros (eles/elas) se não souber.
6. Não prometer número que não mediu. Relate falhas com o texto do log.

## Como trabalhar (comandos)
- Compilar o NRO: `bash runtime/switch/build.sh nro` (Docker devkita64, ~40 s incremental; **+1 em `runtime/switch/BUILD`**, versão `bN` no NACP e no log). Saída `out/switch/nfsu2.nro`. Experimentos (todos 0% no yuzu até agora): `NXGCONST=1`, `NXGLOCAL=1`, `NXO3=1` (shards em `-O3`).
- Rodar no yuzu e medir corrida: `powershell -File runtime\switch\yuzu-bench.ps1 [-Nro out\switch\nfsu2.nro] [-Tag nome]` (~90 s; imprime `gcpu` médio em ms/frame e fps da corrida; acumula em `out/yuzu-bench.csv`). Roteiro: `tests/scripts/nx_quick_race.txt` (perfil CLAUDE -> Quick Race -> 1500 frames -> sai; orientado a eventos).
- Só rodar e ver o log: `powershell -File runtime\switch\yuzu-run.ps1 -Seconds 100 -Nro <nro>` (termina quando o app sai).
- SD virtual do yuzu: `%APPDATA%\yuzu\sdmc\switch\nfsu2\` com `nfsu2.dat`, `tbuf` (OBRIGATÓRIO no yuzu), `captura` (salva `captura.png`), `show.txt` (roteiro), `lento.txt` (K, não calibrado). **Nunca** `software` no yuzu (renderer por CPU, não é o que roda no console). Yuzu configurado para ficar próximo do console: OpenGL, memória 4 GB, portátil, sem "unsafe" (ver `EVOLUCAO.md`; backup `qt-config.ini.bak-before-fidelity`; o yuzu reescreve o arquivo ao abrir).
- Log do yuzu: `%APPDATA%\yuzu\log\yuzu_log.txt`. Erro `LD is not implemented` = pipeline sem desenhar (tela preta): é o SSBO; `tbuf` resolve.
- Ler o console: Shell COM (`New-Object -ComObject Shell.Application`, `NameSpace(17)`, item "Switch" -> "SD Card" -> switch -> nfsu2), `CopyHere(item, 0x14)` para o scratchpad. Dois consoles existem: marcador `switch_oled` (OLED) e `switch` (outro, às vezes desconectado).
- Harness Linux/WSL (para `perf`): `runtime/switch/build-linux.sh` -> `~/nfsu2-linux-build/nfsu2_bench <SPEED2.EXE> <roteiro> <segundos>`; `perf` existe no WSL Fedora (`FedoraLinux-43`, `perf_event_paranoid=2`: só o próprio processo).

## Arquivos de marcação na pasta do app (SD)
`online` (liga a rede), `semroam`, `carros.txt` (nº de carros remotos, padrão 2), `equipe.txt`, `resolucao.txt` (LxA até 1280x720), `software` (renderer CPU), `tbuf` (VB como textura em buffer), `travar`/`unlock`/`restaurar` (preset gráfico), `boost` (NÃO usar: piora), `nopin`, `nocache`, `semvibracao`, `colisao`, `captura`, `lento.txt`, `show.txt` (roteiro; também usa `save_bench` vazio e não abre o teclado).

## Estado das medidas (resumo; detalhes em EVOLUCAO.md)
- OLED b64, 960x540, 2 carros, opções do jogador: corrida ~20 fps, `gcpu` 33-37 ms, `record` 15-21 ms, `swap` 6 ms, GIL 58% em fila. Sem amigos ~39-48 fps.
- O **primeiro carro remoto** custa ~14 ms/frame, os seguintes 1-2 ms. Causa **desconhecida** (carga de modelo? física? hooks?).
- yuzu b67 (4 GB, portátil): corrida `gcpu` ~17,7 ms/f, ~43 fps (sem amigos; o roteiro é offline). Ruído entre rodadas ~0,2 ms.
- Boost de CPU 1785 MHz piora muito (causa não confirmada): fica desligado.

## Experimentos feitos nesta rodada (não commitados)
1. **`NXGCONST=1`**: base `const` → 0% no yuzu (17,65 vs 17,7). Opt-in.
2. **`perf` + `NXGLOCAL=1`**: no Linux x86 o asm de `sub_00583100` passa de 16 cargas RIP de `g_mem` para 1 (base local `restrict` em `LR_ENTER`, `cpu.h`). **Wall-clock 0%** no harness (gcpu 3,04 → 3,08 ms) e no yuzu b71 (17,9 / 18,6 vs base ~17,7). O IP quente era o *acesso* à memória do guest, não o reload da base. Opt-in; **não aplicar ao console**.
3. **`NXO3=1` (shards `-O3`, b72)**: yuzu 18,5 / 17,9 ms — dentro do ruído / pior. NRO maior (37 MB vs 34). Opt-in; **não aplicar**.
4. Ferramentas: `runtime/switch/perf-race.sh`, `perf-resolve2.py`, `yuzu-bench.csv` atualizado.

## O que fazer em seguida (ordem sugerida)
1. **1º carro remoto (~14 ms)** — maior retorno restante. Levantar amigos no yuzu (`roam-pair` / hub com aviso) ou harness; achar o que o 1º carro dispara de O(1). Meta ≥20% no `yuzu-bench` (2× base + 2× nova) antes de copiar ao OLED.
2. GIL (`gil=` no `[status]`, `win32_ext.c`): no OLED 58% na fila; carimbos de tempo / trocas.
3. `record` do GL / preset: b64 rodou com opções do jogador (`[nx] graphics: the player's options...`); comparar só com a mesma linha. `tbuf` no console = teste de `record`.
4. Free roam lista vazia no hub (Switch vê 0, PC vê 16).
5. Dívidas em `DEBITO-TECNICO.md` + limpeza de marcadores do SD virtual no `yuzu-run.ps1`.

## Armadilhas já pagas
- Não ler registradores ARM com asm no Horizon (use `armGetSystemTick`). `rename()` sobre arquivo existente falha na FAT: `unlink` antes. `pread` com fds da libnx quebrava: há camada própria de arquivos (`nx_host.c`, `vfs_dat.c`). Título tomado (R ao abrir um jogo) é obrigatório: applet só dá 403 MB.
- `sed` com `\r`/`\n` em caminhos do Windows: use Edit. PowerShell: variáveis sem distinção de caixa; `Del` é Remove-Item.
- O certificado do TLS deste PC é interceptado pelo antivírus: não pegar certificados dele; o app tem as raízes Mozilla embutidas.

## Resultado da rodada de otimização (2026-11-02) — atualização
- Perfil `perf` da thread do jogo: distribuição plana (ver `EVOLUCAO.md`, seção "Rodada de otimização"); `NXGCONST` não rendeu nada. **Nenhuma build foi copiada ao console.**
- Para refazer o perfil: `wsl -- bash -lc 'cd <repo in WSL> && bash runtime/switch/build-linux.sh'` e depois, em `~`, `perf record -F 400 -g -o perf.data ~/nfsu2-linux-build/nfsu2_bench /mnt/d/Games/NFSU2/SPEED2.EXE ~/q.txt 150` (com `NFSU2_SAVEDIR=$HOME/lsave NFSU2_REGFILE=$HOME/lreg`); `perf script -F tid,ip,dso` + `nm -n` para nomear (o `perf report` não resolve os símbolos deste binário). Separe por `tid`: o harness Linux usa o rasterizador por software, que no console não existe.
- Próximo candidato de maior retorno: o 1º carro remoto (precisa de amigos no yuzu/harness: `roam-pair`), depois o GIL (trocas + carimbos de tempo em `runtime/win32_ext.c`), depois `record` do GL.

## b78: GPU overclock + English flag names (2026-11-02)
- `gpuclock.txt` (one number, MHz, clamped 307..1075; absent/0 = system clock). nx_main.c `gpu_clock_apply` sets it via clkrst at start and every ~100 loop ticks (the system gives the clock back on dock/sleep), restores on exit; the log prints `[nx] gpu clock ...` and the `[nx] t=` lines show the real `gpu=` MHz. Release ships `gpuclock.txt` = 768.
- Flag files renamed to English (old names are renamed on the SD at start, `migrate_flag_names`): captura->capture, lento.txt->slowdown.txt, resolucao.txt->resolution.txt, semroam->noroam, carros.txt->cars.txt, equipe.txt->crew.txt, colisao->collide, travar->lockgfx, restaurar->resetgfx, semvibracao->norumble, endereco.txt->address.txt, servidor.txt->server.txt, nome.txt->name.txt, relatorio(.txt)->report(.txt). Older sections of this file / EVOLUCAO / DEBITO-TECNICO still use the Portuguese names.
- Deployed to the OLED as nfsu2_b78.nro + gpuclock.txt (versioned name: MTP does not overwrite). Compare race fps/gcpu with 0 vs 768 vs 921.
- b78 OLED result (960x540, 0 friends, race, handheld): GPU 307 MHz (system default, gpuclock.txt absent) -> race ~20 fps, gcpu ~33 ms; 768 MHz -> ~36 fps, gcpu ~20 ms; 921 MHz -> ~36.5 fps, gcpu ~20 ms. The game thread waited on the GPU at 307; from 768 up it is CPU-bound (no gain at 921). Keep 768. Not yet measured with friends in the race.

## b89: Online bug fixed + validated yuzu online test (2026-11-02)
- BUG (lan_nx.c `attach`): a LAN mailbox that came up AFTER the server's welcome got `publish_online()` but not `publish_peers()`, so the game saw no Online host (10.77.0.1) -> "no such host on the switch" -> lobby "connecting" -> UI_OL_Disconnect. Fixed (also in the console, where it depended on the order mailbox/welcome).
- Test: `powershell -File runtime\switch\yuzu-online-test.ps1` (exit 0/1, PASS/FAIL per check, `-NegativeControl` kills the hub and must FAIL). Uses the console's profile <profile> (copied into the yuzu SD save/), script tests/scripts/nx_online_rankings.txt (user's timing: 7 s, 5 s confirm, 1 s, RIGHT+confirm = Online, 7 s lobby, RIGHT RIGHT + confirm = RANKINGS), waitfng gates for yuzu's slower boot. b89: ALL PASS; negative control: 5 checks fail as it must.
- yuzu needs `network_interface=Ethernet` in qt-config.ini (it was "Nenhum": every guest socket() asserted). yuzu's getaddrinfo returns socktype 0 (n2n_sock.c forces SOCK_STREAM) and answers a non-blocking connect with EAGAIN (accepted).
- Flag files: `keepsave` (script runs on the real save/ instead of an empty save_bench), `fnglog` (log every screen change), `padlog` (every pad press with time/frame/screen, to turn a hand-played run into a script), `gpuclock.txt`.
- The local hub dies with its wsl session: runtime/switch/hub-keep.sh is started by the test with a hidden long-lived wsl.exe. yuzu-roam-bench.ps1 does NOT use it yet and prints a gcpu even when nothing connected: do not trust its numbers until it asserts the connection.

## b92: NSP fullbuild + release/nro + release/nsp (2026-11-02)
- NSP: runtime/switch/build-nsp.sh -> release/nsp (2.08 GB: NSO + nfsu2.dat in the RomFS). Details, hacBrewPack fork and sha256: docs/switch/NSP.{pt-BR,en}.md. vfs_dat.c reads the dat from the RomFS when there is one; online is on by default in the NSP (file offline turns it off). NPDM cores must be 0-2 (core 3 hung the main thread).
- Layout: release/nro/switch/nfsu2 (copy to the SD root) and release/nsp, each with README.pt-BR.md + README.en.md. yuzu-run.ps1 points to release
ro.
- yuzu validation, all PASS at b92: existing profile -> RANKINGS; -Fresh -Package nro; -Fresh -Package nsp; -NegativeControl FAILS as it must. NOT yet installed on the console (NSP).
- Flag segurar is now holdturn. Leftover in the yuzu SD: switch
fsu2x (the other port data, 2.5 GB) can be deleted.

## b95: name, credits, loading logo (2026-11-03)
- NACP (NRO and NSP): name "Need for Speed: Underground 2", author "Electronic Arts" (build.sh, runtime/switch/nsp/mk-nso.sh). NSP file: "Need for Speed Underground 2 [01004E4653550000][v0].nsp". The app screen says the full name too.
- The "Nintendo Switch" loading animation: build-nsp.sh no longer uses --nologo; it needs out/nsp/logo/{NintendoLogo.png,StartupMovie.gif} (Nintendo files, extracted with hactool from section 2 of a homebrew NSP Program NCA; not in the repo). hacBrewPack reports "Program NCA Logo Section: Yes". yuzu does not show that animation: only a console can confirm it visually.
- NSP inputs moved into the repo: runtime/switch/nsp/{app.json,mk-nso.sh,build-hacbrewpack.sh}. Credits to EA in the READMEs. yuzu fresh tests (nro and nsp) PASS at b95.
- release/ root now has README.pt-BR.md + README.en.md (the old LEIA-ME.txt, build 64, was replaced).
