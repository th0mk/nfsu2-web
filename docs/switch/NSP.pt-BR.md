# Gerar e testar o NSP do NFSU2 ("fullbuild")

O NSP é um título instalável (`01004E4653550000`): o programa como NSO mais o `nfsu2.dat` no RomFS, assinado como os NSP de homebrew.
Ao iniciar, `vfs_dat_mount_romfs` (runtime/switch/vfs_dat.c) procura o `nfsu2.dat` na raiz do RomFS do processo e lê com `fsStorageRead`; um NRO não tem RomFS e
cai em `sdmc:/switch/nfsu2/nfsu2.dat`. No NSP o online liga sozinho (sem o arquivo `online`), a menos que exista um arquivo `offline`.

## Gerar

Precisa de Docker (imagem `devkitpro/devkita64`), de `release/nro/switch/nfsu2/nfsu2.dat` e de três coisas em `out/nsp/` (ignorado pelo git):

1. `hacbrewpack`: o hacBrewPack compilado na imagem. O repositório original (The-4n/hacBrewPack) saiu do ar; o fork usado é
   `github.com/dragonflylee/hacBrewPack` (tarball do master, sha256 `21726242a982b702d8d000a98fe77fafd3771668029dee62db069512e7b7e03b`), compilado com `make` e copiado para `out/nsp/hacbrewpack`
   (`runtime/switch/nsp/build-hacbrewpack.sh` faz isso dentro do container). GPL-2.0.
2. `keys-min.txt`: SÓ as linhas `header_key` e `key_area_key_application_*` do seu `prod.keys` (o leitor do hacBrewPack rejeita outras linhas de um arquivo completo).
   **Nunca faça commit de chaves**; `out/` é ignorado pelo git.
3. `logo/NintendoLogo.png` e `logo/StartupMovie.gif`: a animação "Nintendo Switch" mostrada enquanto o título carrega. São arquivos da Nintendo e **não estão neste repo**; extraia-os
   do Program NCA (seção 2) de qualquer NSP que você tenha, p. ex. `hactool -k keys-min.txt -t nca --section2dir=out/nsp/logo <program.nca>`. Sem eles o hacBrewPack faz uma tela de carregamento preta.

Depois:

```
runtime/switch/build.sh nro        # o ELF (out/switch/nfsu2.elf) e o NRO
runtime/switch/build-nsp.sh        # NSO + NPDM (runtime/switch/nsp/app.json) + NACP + ícone -> hacBrewPack -> release/nsp/Need for Speed Underground 2 [01004E4653550000][v0].nsp
```

`runtime/switch/nsp/app.json` é o NPDM do template do hacBrewPack com: title id `0x01004e4653550000`, espaço de endereços de 39 bits, **núcleos 0-2** (o 3 é do sistema: uma thread principal nele nunca rodou),
threads 24-63 e as SVCs que o runtime usa (`CreateCodeMemory`, `ControlCodeMemory`, `CreateSharedMemory`, ...).

## Testar (yuzu, antes de qualquer console)

O yuzu precisa de `network_interface=Ethernet` no `qt-config.ini` (vem como "Nenhum": todo `socket()` do jogo falha) e de `prod.keys` + firmware.

```
powershell -File runtime\switch\yuzu-online-test.ps1                        # um perfil existente -> Online -> RANKINGS (timing do usuário)
powershell -File runtime\switch\yuzu-online-test.ps1 -Fresh -Package nro    # DO ZERO: release\nro copiado para um SD vazio, perfil novo
powershell -File runtime\switch\yuzu-online-test.ps1 -Fresh -Package nsp    # DO ZERO: release\nsp aberto como título, nada no SD
powershell -File runtime\switch\yuzu-online-test.ps1 -NegativeControl       # hub desligado: o teste tem que FALHAR
```

Cada execução imprime PASS/FAIL por verificação, com a linha do log como evidência, e pede ao hub local (WSL, `hub-keep.sh`) que confirme a entrada de forma independente. O `-Fresh` move a pasta do SD do yuzu para o lado e a devolve;
o último log fica em `out/nsp/last-run-<nro|nsp|existing>.log`. Flags só de teste: `tbuf` (limite de shader do yuzu), `fnglog`, `padlog`, `keepsave`, `capture`, `slowdown.txt`, `show.txt`.
Um perfil novo é enviado primeiro à escolha de carro do Online (`UI_OLCarSelect`); por isso o `-Fresh` aceita essa tela onde o perfil existente chega em `UI_OLRankings`.

## Limites conhecidos

- O NSP foi validado só no yuzu; a primeira instalação num console é o teste real (precisa dos patches de assinatura, como qualquer NSP de homebrew).
- O NSP leva os dados do jogo: é para a sua cópia do jogo, não para distribuir.

## Nome e créditos

O título é **Need for Speed: Underground 2**, autor/publisher **Electronic Arts** (NACP, no NRO e no NSP). O jogo, o nome e os dados pertencem à Electronic Arts; este é um port não oficial.
