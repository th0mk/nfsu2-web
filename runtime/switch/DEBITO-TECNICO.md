# Débito técnico do port para Nintendo Switch

Estado em 2026-11-02, build **b28** (`/switch/nfsu2/nfsu2_bNN.nro`). O que funciona hoje, medido no console (OLED, Atmosphere,
título tomado com R+A): o jogo inteiro carrega, menus a 60 fps, Quick Race a 40-49 fps (preset médio) ou ~20 fps (gráficos
máximos), renderizador OpenGL 4.3 (Mesa/NV120) com 0 falhas de shader, controle do console, ícone, saída limpa. Cada item abaixo
diz **o que se sabe** (fato do log), **o que se supõe** e **como verificar**.

Legenda de prioridade: **P1** atrapalha jogar, **P2** deveria ser feito, **P3** polimento / só se sobrar tempo.

---

## 1. Online (P1: é o objetivo original)

**Estado:** não integrado ao jogo. O cliente WebSocket existe e está testado fora do console: `homebrew/switch-net/`
(`n2n.c/.h`, `n2n_sock.c`, `n2n_tls_mbedtls.c`, `n2n_cli.c`, README, `test/fake-switch.mjs`, `test/run.sh`). Fala o protocolo `/lan`
do servidor, testado contra um servidor falso (ASan limpo) e contra `wss://nfsu2.xyz`.

**O que falta:**
- Ligar o cliente à camada de rede do runtime no lugar do "mailbox" do navegador (`runtime/web/host_abi.h`; hoje `nx_host.c`
  tem só stubs: `nfsu2_register`, `nfsu2_host_mailbox`). É preciso ler como o jogo vê a rede (winsock emulado no runtime) e como
  o port web a entrega ao servidor, e alimentar o mesmo contrato.
- Sockets no console: a primeira versão chamava `socketInitialize` e o app **caiu** (sphaira / bsdsocket); foi removido. Terá que
  voltar, com a configuração de memória/serviço correta (`SocketInitConfig`) e o módulo TLS (mbedtls já está no cliente).
- Certificados: `mbedtls` precisa de uma cadeia de CAs no cartão SD (ou pinar o certificado do `nfsu2.xyz`).
- Perfil/nick/login: o servidor espera o fluxo do site (passphrase de nuvem, etc.); ver `docs/web/PLAYING.md` e `docs/web/VPS.md`.
- Teste ponta a ponta: duas máquinas (Switch e navegador) na mesma sala.

**Verificar primeiro:** conectar o `n2n_cli` do PC a `nfsu2.xyz` (já feito) e depois rodar o mesmo handshake dentro do NRO
(um modo "só rede", sem o jogo) para provar o socket e o TLS no console antes de ligar ao jogo.

---

## 2. Gráficos: o preset não é respeitado direito (P1)

**Fatos (log do b27):**
- O arquivo `/switch/nfsu2/.nfsu2_registry` fica **correto** (28 linhas: reflexos de carro e pista, motion blur, profundidade de campo,
  overbright e bleach bypass = 0; `g_RacingResolution=0`; sombras 2 etc.). Mensagem: `graphics: Switch preset (medium, 720p)`.
- Mesmo assim o usuário relata que o preset "não parece respeitado"; ajustando à mão no menu o resultado ficou ótimo.
- O jogo faz **88 chamadas `RegSetValueExA`** logo ao iniciar. Com `NFSU2_LOCK_GFX` as escritas `g_*` são descartadas
  (`win32_ext.c`), mas o jogo grava de volta o que ele *acha* que são as opções, então o valor em memória pode divergir do arquivo.

**Hipóteses (nenhuma confirmada):**
1. O jogo lê as opções gráficas **de outro lugar** (outro nome de valor, ou uma tabela interna calculada pela "verificação do sistema"
   da primeira execução: `0x5CED60 -> 0x5CEC90`, MHz da CPU + pontuação da GPU → `g_PerformanceLevel`) e o registro só vale para
   parte delas.
2. A ordem: o jogo lê os valores **antes** de o preset ser gravado (o arquivo já foi lido em `reg_load` numa chamada anterior e o
   cache `g_reg` do runtime não é recarregado).
3. O jogo recomputa as opções a partir de `g_PerformanceLevel` (preset 5 = máximo) depois de ler as individuais.

**Como verificar:** rodar com `REGLOG=1` (loga `RegQueryValueExA`/`SetValue` por nome) e comparar, valor a valor, o que o jogo lê com o
arquivo; abrir Options > Display e anotar o que a tela mostra após uma abertura limpa. No PC, `NFSU2_REGFILE=<arquivo>` reproduz o
mesmo cenário sem o console. Se for a hipótese 3, o preset deve baixar `g_PerformanceLevel` (testar 2 ou 3).

**Relacionado:** o preset só vale **a cada abertura**. Dentro da sessão a tela de opções ainda muda as opções (só não são
salvas). Travar de verdade pede interceptar o tratador das opções do jogo (endereços ainda não mapeados) ou esconder a tela.
`unlock` (arquivo vazio em `/switch/nfsu2/`) desliga tudo.

**Ideia de melhoria de custo:** com `NFSRES=1280x720` **todos os render targets** viram 1280x720
(`[RT-tex] 640x480 -> 1280x720 (NFSRES)`), inclusive os de reflexo e cubemap, que no original são pequenos. Escalar só o alvo final
(ou os alvos grandes) e deixar os auxiliares pequenos deve tirar muito custo de preenchimento nos gráficos altos.

---

## 3. Desempenho

**Fatos medidos:**
- Corrida, preset sem reflexos: **40-49 fps**; thread do jogo a 100% de um núcleo a 1020 MHz (`gcpu` 19-22 ms/frame); renderizador `record` ~5,5 ms,
  `swap` ~0,4 ms. **O limite é a CPU do jogo (código x86 traduzido para C), não a GPU.** GPU a 307 MHz, EMC 1331 MHz.
- Gráficos máximos: ~20 fps, ~30 passes de render por frame, `gcpu` 35-43 ms, `record` 15-28 ms.
- **O boost de CPU a 1785 MHz piorou muito** (16-21 fps contra 38-44 sem ele). Causa **não confirmada**. Hipótese: orçamento de energia
  compartilhado (CPU/GPU/EMC) no modo portátil baixa o clock da GPU/memória. O log do b22+ mostra `cpu/gpu/emc` MHz por linha; falta
  um teste com o arquivo `boost` ligado para ver o `gpu=` e o `emc=` sob boost. Hoje o boost vem **desligado** (só liga com o arquivo `boost`).
- O laço de desenho do renderizador é só ~3 ms de ~40 (b21); o custo estava em outro ponto do frame (cópias de dados / passes), e a
  parte pesada era o boost, não o laço.

**A fazer:**
- Descobrir por que o boost piora (log de clocks com `boost`); se for o orçamento de energia, estudar `appletSetCpuBoostMode`/perfil
  sem mexer no clock manualmente, e o modo **dock** (mais energia disponível: o ganho pode ser diferente).
- Otimizar o código traduzido: `-O2` hoje; testar `-O3`, `-flto`, `-fprofile-use` (PGO com as fases quentes), `-mcpu=cortex-a57`
  (verificar se já está), reduzir indireção nas chamadas traduzidas, e ver `gil=` no `[status]` (contenção do lock global entre as 5 threads).
- Hitching: cada programa de shader novo compila em 40-185 ms **no meio da corrida** (log `[glr] program ... ok in N ms`). Salvar os
  binários (`glGetProgramBinary`) no SD e pré-aquecer, ou compilar na tela de loading.
- O alvo de 60 fps depende de reduzir ~30% do custo do jogo; depois de 1 e 3, avaliar um limitador de 30 fps estável no lugar de
  40-49 irregulares (melhor sensação).
- Remover ou proteger por variável a instrumentação `prof(...)` do b20+ (custo pequeno, mas é ruído).

---

## 4. Saída do app e estabilidade

- **Tela de erro 2168-0006 ao sair** (b22): `User Break` na thread do jogo, causado por `main` retornar enquanto a thread do jogo ainda lia
  arquivos pelos serviços que a libnx fechava. b24 chama `svcExitProcess()` ao sair com o jogo rodando; o usuário ainda via um erro
  igual, **mas não há relatório novo do Atmosphere do b24**, então não está provado que é o mesmo crash. b28: quando o jogo termina sozinho
  (Minus -> confirmar, ou Quit Game) o app sai na hora, sem a tela preta de "Done" (o usuário achava estranho). **Verificar** no b28 se
  sobra algum erro, e ler o relatório (`/atmosphere/crash_reports`, tradução de endereço:
  `aarch64-none-elf-addr2line -f -C -e out/switch/nfsu2.elf 0xENDERECO`, no contêiner `devkitpro/devkita64`).
- **`crash.txt` não é confiável:** o `__libnx_exception_handler` usa `fopen` dentro do contexto de exceção, e isso já falhou. Reescrever
  usando `fsFileWrite` direto, sem malloc, ou gravar num buffer estático e salvar ao sair.
- **Congelamento total do console no b13** (primeiro teste do GL) sem relatório algum. Não reproduzido depois do endurecimento do
  b14 (anel de 3 buffers, folga nos buffers, slots de PS antes do uso). A causa real é desconhecida. O monitor de 2,5 s
  (`no frame for N s`) continua útil; o arquivo vazio `software` cai para o renderizador por CPU (b10-style, **sem manutenção**).
- Ruído: o monitor imprime `no frame for 2.5 s: the renderer is idle` uma vez durante o carregamento da corrida (inofensivo).

---

## 5. Entrada

- **Mapeamento**: "quase OK" segundo o usuário; sem defeito explícito. Está em `CONTROLES.txt` e em `nx_main.c:pad_to_game()`
  (por rótulo do botão: A confirma/B volta; ZR/ZL acelera/freia; L nitro; R freio de mão; A/X marchas; B ré-olhar; Y câmera; Minus quit
  prompt/reset; Plus pausa; **Minus+Plus 1 s sai**). Pontos a rever com uso real: tudo-ou-nada dos gatilhos (sem analógico), se Minus deve
  ser "sair" nos menus, zona morta de 12%.
- **Digitar texto (nome do perfil) não existe**: exige teclado. Solução natural: `swkbd` da libnx chamado quando o jogo abre um campo de
  texto (o jogo recebe `WM_CHAR`; ver `dinput_pad.c fe_keys`). Sem isso não dá para criar perfil/carreira; só Quick Race sem perfil
  (a tela mostra "No Profile").
- Sem **vibração** (o runtime já mistura os efeitos de FF do jogo em dois motores: `gamepad_rumble`; falta ligar a `hid` vibration da libnx).
- Sem toque na tela, sem giroscópio, sem segundo jogador local, sem controles com mapeamento personalizável fora do jogo.
- Hotplug: `connected` é sempre 1; se o controle desconectar, o jogo não percebe.

---

## 6. Áudio e vídeo

- **Áudio: não existe.** `audio_web.c` é excluído do build e a `null_platform` responde sem som. Falta uma saída com `audout`/`audren`
  da libnx alimentada pelo mesmo contrato dos buffers do jogo (`audio_*` em `runtime/web/`). Prioridade alta para a experiência.
- **Vídeos (`MOVIES/*.vp6`) não rodam**: o log mostra `CANNOT OPEN MOVIES/ealogo.vp6, FMVOpening, PSA, THX_logo`. Pode ser só ausência dos
  arquivos na cópia do SD (ver item 8) ou falta de decodificador (o port web decodifica vp6 no navegador). Hoje o jogo passa direto.

---

## 7. Cópia do jogo no SD e dados faltando (P2)

- **Corsa e Peugeot 106 (carros europeus) não existem na cópia do SD** (`CARS/CORSA`, `CARS/PEUGOT106` faltando), embora o código
  `regional.c` suponha que a pasta de jogo os tenha. O b16 faz o runtime tirá-los das listas quando faltam (sem isso o jogo carregava
  para sempre). **Verificar** se a pasta original `D:\Games\NFSU2` tem esses arquivos e se a cópia (1256 arquivos, ~1,7 GB) os perdeu;
  comparar listagens e copiar o que faltar.
- `NFSUNDER/ZDIR.BIN` também falta (o jogo continua), e os `MOVIES/*.vp6` (item 6).
- A cópia foi feita por MTP (DBI), que não dá tamanhos/datas confiáveis. Gerar uma lista de verificação (nome + tamanho) da pasta
  original e comparar com o SD depois de copiar.

---

## 8. Implantação, ferramentas e higiene do repositório (P2)

- **Nada disto está commitado.** O `git status` do repositório tem `runtime/switch/`, `homebrew/switch-net/`, as edições em `runtime/*.c`,
  `runtime/web/gpu_web.c`, `runtime/regional.c` etc. fora do histórico. Fazer commits por assunto (n2n, port base, renderizador GL,
  entrada, gráficos, ícone).
- **MTP (DBI)**: apagar arquivos **trava**; copiar por cima de um nome existente "emite" mas **não substitui**. Por isso cada build tem nome
  novo (`nfsu2_bNN.nro`) e o usuário apaga os antigos no Explorer. Alternativa mais limpa: um servidor FTP no console (ftpd) ou
  `nxlink` (rede do console) para enviar o NRO e ler o log sem o DBI.
- `LEIA-ME.txt` no SD está **desatualizado** (diz que existe um único `nfsu2.nro`); `CONTROLES.txt` é o texto atual. Unificar.
- `runtime/switch/icon.jpg` foi gerado por `make-icon.ps1` a partir de uma captura do menu (`out/shots/gl_last.png`, que **não é versionada**).
  Versionar o `icon.jpg` gerado (já está em `runtime/switch/`) e documentar. O logotipo é marca da EA: **não distribuir publicamente** o NRO/ícone.
- Mensagens de commit/build: `BUILD` numera por `nro`; o NACP leva `bNN`. Manter.
- `LEIA-ME`/docs em português para o usuário; os comentários do código seguem o padrão em inglês do repositório.

---

## 9. Modo e memória

- Só roda com **título tomado** (segurar R e apertar A em um jogo): ~3,1 GB disponíveis. Em modo applet (Álbum) só ~400 MB e o `mem_init`
  de 640 MB falha; o app mostra uma mensagem explicando. Rodar do menu de homebrew normal exigiria reduzir a memória.
- O campo `rss=` do `[status]` no console mostra valores sem sentido (25-53 GB); medir memória real pelo `svcGetInfo` se for preciso.
- A janela padrão do console vem com tamanho 0x0 (`nwindowSetDimensions(1280,720)` foi o conserto do b15) e o renderizador força
  1280x720 também; **não suporta dock em 1080p** (o desenho é 720p de qualquer modo; a saída do dock é escalada pelo sistema).

---

## 10. Testes

- Há um laço de verificação no **PC** (WSL Fedora + Mesa llvmpipe, `runtime/switch/build-linux.sh`, `host_linux.c`) que roda o mesmo
  renderizador GL e salva capturas, e `tests/shader_to_glsl.c` (83/83 shaders compilam em GL 4.3 core). **Não cobre**: controle do console,
  `NFSRES` do menu, sistema de arquivos do cartão SD (o `rename` por cima de arquivo existente só falhou no console), clocks, serviços.
- Nenhum teste automático do caminho do console. Valeria um modo "autoteste" no NRO (usa `show.txt`/`switch_probe.txt` como script de
  entrada, já suportado) com um relatório final no log (telas visitadas, fps por fase, falhas de shader).
- Regressão do renderizador: guardar capturas de referência (menu 3D, corrida) e comparar na mudança do renderizador.

---

## 11. Riscos e decisões abertas

- **Fidelidade x fluidez:** o usuário achou o máximo "jogável" (~20 fps) e o médio "supimpa" ajustado à mão. Decidir o padrão final: travar
  em médio (recomendado: ~45 fps) ou em algo entre os dois, e se mantém o `unlock` para quem quiser o máximo.
- O renderizador GL depende de um driver (Mesa 20.1 rc3/NV120) de 2020: extensões usadas (`clip_control` via `eglGetProcAddress`,
  `shader_draw_parameters`, `buffer_storage`, S3TC, BPTC). Mudanças de firmware/Atmosphere podem alterar isso.
- Compatibilidade com outras revisões de console (Switch comum, Lite, Mariko) não foi testada: só o OLED do usuário.

---

## Atualização (b40, 2026-11-02): o que mudou nos itens acima

- **Item 2 (preset "não respeitado"): causa achada.** O preset era respeitado, mas era mais pesado que o padrão do jogo. Medido no PC na mesma
  corrida (passes de render por frame): registro vazio (o que o b22 tinha, jogo em padrões baixos) **1**; primeiro preset **4**; rastros de luz
  desligados **2**; neblina/chuva desligadas **3**; colorização+plateia+partículas+textura animada desligadas **3**; sombras, nível de detalhe e
  `g_PerformanceLevel` **sem efeito** nos passes. O preset do b40 (rastros, colorização e plateia desligados) mede **2**. Falta confirmar no console
  o ganho de fps (esperado entre os 28 fps do b37 e os 40-49 do b22). O in-session lock continua pendente.
- **Item 1 (online):** não iniciado.
- **Item 4 (saída):** b39+ pede o fechamento ao jogo (`lifecycle_request_quit` + `lifecycle_pump`, WM_CLOSE), deixa gravar os arquivos abertos, para o áudio e o
  renderizador e **retorna de `main`** (sem `svcExitProcess`, que gerava erro no console). Só cai para o `svcExitProcess` se o jogo não fechar em 15 s. Falta confirmar no console.
- **Item 5:** teclado do sistema **pronto** (`text_nx.c`; funcionou no b37). Vibração e gatilhos analógicos continuam pendentes.
- **Item 6:** **áudio pronto** (`audio_nx.c`, audout 48 kHz; funcionou no b37; 7 underruns no início de cada execução, ver se o `target` precisa subir). Vídeos continuam ausentes.
- **Item 7:** o PC **não tem** `CARS/CORSA` nem `CARS/PEUGOT106` (não é falta da cópia); o `.dat` tem os mesmos 1256 arquivos da pasta original.
- **Item 8/9:** o jogo agora vai num **`nfsu2.dat`** único (1705,8 MB, `pack-dat.mjs`, testado no PC com `tests/dat_test.c`); o app confere cabeçalho, tamanho e CRC do índice ao abrir.
  A cópia por USB leva ~140 s e dá para conferir o tamanho pelo MTP (`ExtendedProperty('System.Size')`, `out/switch/copy-to-sd.ps1`).
  Ainda vale: relatório `relatorio.txt` gravado a cada execução (fps por fase, áudio, texto, falhas de shader).

---

## Atualização noturna (b45, 2026-11-02): trabalho feito sem dados do console

Tudo abaixo **compila e foi testado no PC onde dava** (WSL: teste de rede contra servidor falso e contra o `nfsu2.xyz` real; cache de shaders;
render do contador de fps). **Nada disto foi executado no Switch**: o `nfsu2_b45.nro` está em `out/switch/pack/`, não foi copiado para o cartão (regra:
o repositório é a minha sandbox). O `nfsu2_b41.nro` (o último que você testou) continua sendo a versão segura.

| Item do documento | O que foi feito | Chave para desligar | Como saber no log/relatório |
|---|---|---|---|
| 3. Corrida lenta com som (28 fps contra 40-49 sem som; `gil` 100% enfileirado) | Hipótese: threads de som e do renderizador tirando a thread do jogo do núcleo dela. Agora: thread principal do jogo no **núcleo 2** sozinha, renderizador no 1, som no 0, as demais no 0/1 (`nx_host.c`, `__wrap_pthread_create`). | arquivo vazio `nopin` | `relatorio.txt`: "core masks asked: 0x4->0x0 ..." (0 = aceito) e fps da corrida |
| 3. Travadinhas de shader (40-185 ms) | **Cache de programas** do driver em `/switch/nfsu2/progcache.bin` (glGetProgramBinary/glProgramBinary; chave = hash dos dois GLSL; cabeçalho = versão do GL). Testado no PC: 13 programas carregados do arquivo na 2ª execução, imagem idêntica. | arquivo vazio `nocache` | log "program ... (from the cache)"; relatório "program cache: ... loaded/saved" |
| 4. `crash.txt` não confiável | O tratador de exceção agora não usa stdio nem malloc: formata num buffer estático e grava pelo serviço de arquivos (`fsFileWrite`), com o número do build. | - | `crash.txt` após uma queda |
| 5. Sem vibração | O mixer de force feedback do jogo (`dinput_pad.c`) agora chega aos atuadores dos Joy-Con/Pro via HID (banda baixa 160 Hz = motor forte, alta 320 Hz = fraco); para sozinho quando o jogo para de pedir e na saída. | arquivo vazio `semvibracao` | log "[nx] rumble: handheld N, joy-con pair N, pro controller N" |
| 3. Sem visibilidade do custo de leitura do `.dat` | `relatorio.txt` agora traz, por fase (menu, loading, corrida): leituras, MB, ms totais e a maior. | - | "file reads in flow N: ..." |
| 1. **Online** | **Camada de rede pronta** (`lan_nx.c` + `homebrew/switch-net`): conecta por WebSocket seguro ao `wss://nfsu2.xyz/lan` quando o jogo pede (campo `want`), publica endereço/peers/estado nas caixas de correio do jogo, envia o que o jogo enfileira e entrega o que chega (free roam e LAN). Certificados: as 118 raízes da Mozilla estão embutidas (`cacert_pem.c`, gerado por `make-cacert.mjs`). O `hello` agora diz que o LAN play está aberto (`n2n_set_lan`) e o app avisa quando abre/fecha. **Testes no PC:** `tests/lan_test.c` + `tests/fake-lan.mjs` (11 verificações OK: online, vip/netmask, peers, RX/TX dos dois anéis, tráfego de aviso não vai à rede, ocioso e reconexão) e conexão TLS real ao `nfsu2.xyz` (recebi vip, peers e quadros de outros jogadores). | só liga com o arquivo vazio **`online`** em `/switch/nfsu2/` | log `[net] ...`; relatório "network: ..." |

### Para ligar o online (quando você acordar)
1. Copie o `nfsu2_b45.nro` e crie o arquivo vazio `/switch/nfsu2/online`. O servidor padrão é `wss://nfsu2.xyz/lan`; outro servidor: primeira linha de `/switch/nfsu2/servidor.txt`.
2. Com `online`, o app liga `NFSU2_ROAM=1` (free roam, 16 carros de amigos), tira o `NFSU2_TIMEOUT` (o free roam o confunde com modo de teste) e inicia os sockets (`socketInitializeDefault`) e a thread de rede.
3. O Wi-Fi do console precisa estar conectado. O jogo só pede a rede quando você entra em free roam, ou nas telas LAN/Online.

### O que NÃO foi feito (e por quê)
- Painéis de aviso/lobby/chat/amigo do free roam (tipos 16-19 do anel TX): hoje são descartados. Eram desenhados pela página.
- Codificações compact/delta do servidor: o `hello` não as pede (quadros simples, mais banda).
- Fila de segmentos TCP quando o anel de RX do LAN enche, e recusa automática de `connect` sem endpoint: descartados.
- Descobrir o nome/identidade do jogador: o nome é fixo "Switch"; o `id` é um aleatório persistido em `/switch/nfsu2/id.txt`.
- Teste ponta a ponta do jogo em rede (duas máquinas): só dá com o console.

---

## Atualização (b64/b65, 2026-11-02): log do OLED lido, yuzu como banco de testes

Ver a linha do tempo completa em `EVOLUCAO.md`. O que mudou neste documento:

- **Item 3 (desempenho), números novos do b64** (OLED, 960x540, 2 carros, opções do jogador): corrida 20,5 fps (2-4 amigos: 20-21), `gcpu` 33-37 ms,
  CPU a 84%, `record` 15-21 ms, `swap` 6 ms, GIL 58% na fila. Sem amigos o b52 fazia ~39 fps. O primeiro carro remoto custa ~14 ms, os
  seguintes 1-2 ms cada (ainda sem causa: modelo/física/hooks?). **A fazer:** perfilar a entrada do primeiro carro remoto (yuzu + `lento.txt`).
- **Item 2 (gráficos):** o log do b64 diz `"the player's options (the preset was written before)"`: o preset só vale na primeira vez/"restaurar". Quem comparar fps
  entre builds precisa conferir a linha `[nx] graphics:` antes (o b62 e o b64 não são comparáveis).
- **Item 10 (testes):** yuzu 1734 em `F:\yuzu` roda o NRO inteiro (dat, GL, áudio, rede). Roteiros novos e calibração em `EVOLUCAO.md`.
  Roteiro de corrida automático: `tests/scripts/nx_quick_race.txt`. Falta: roteiro de free roam com amigos (servidor local) e `lento.txt` calibrado.
- **Item 1 (online):** free roam continua com 0 jogadores na lista do hub (suspeita no filtro do hub por campos do HELLO/SNAP). Sondagem no WSL vê 16.
- **Item 8:** `capture_nx.c`, `fsutil_nx.c`, `slow_nx.c`, `yuzu-run.ps1` e os roteiros ainda não estão commitados.

### Atualização b66 (yuzu)
- **Resolvido (ferramenta):** tela preta e teclado travando no yuzu. Ver "b66" em `EVOLUCAO.md` (marcador `tbuf`, roteiros sem swkbd).
- **Novo, P3:** testar `tbuf` no console (textura em buffer em vez de SSBO): pode mudar o custo de `record`/`swap`; hoje só é usado no yuzu.
- **Novo, P2:** `yuzu-run.ps1` não limpa os marcadores do SD virtual (`tbuf`, `captura`, `software`, `lento.txt`): quem roda confere a pasta antes (roteiro com `software` ligado deixa o yuzu sem GL real).

### Atualização b71-b72 (otimização da thread do jogo)
- **Item 3:** tentativas `NXGCONST`, `NXGLOCAL` (base local `restrict`) e shards `-O3` → **0%** no yuzu (ver `EVOLUCAO.md`). Não copiar. Próximo ataque de CPU: custo do **1º carro remoto** (~14 ms) e GIL; faltam amigos no banco yuzu (`roam-pair` ou hub com aviso).
