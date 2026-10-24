# Plano: BOTWRecompAndroid

Recompilação estática de *The Legend of Zelda: Breath of the Wild* (Nintendo Switch, **v1.0.0**) para Android, a partir do motor do [LAS Recomp](https://github.com/peiikai/LASRecomp) e do seu
porte para Android ([LASRecompAndroid](https://github.com/mamcjr/LASRecompAndroid)). Como no LAS, não é emulação:
o código ARM64 do jogo vira C, compilado para a máquina, e o NVN (a API gráfica do Switch) é traduzido para Vulkan.

O plano do porte do LAS para Android, com as medições e decisões dele, está em
[docs/las/ANDROID-PORT.md](las/ANDROID-PORT.md). O README original do LAS está em [docs/las/](las/).

Proposta de um backend opcional de execução ARM64 direta no Android, com etapas,
limites e critérios de validação: [ANDROID-ARM64-EXECUTION.md](ANDROID-ARM64-EXECUTION.md)
(em inglês; proposta de arquitetura, ainda não implementada).

## Ponto de partida

O que vem pronto do LAS e não depende do jogo:

- **O gerador** (`tools/gen.py`, `lifter.py`, `nso.py`, `analyze.py`, `simd_c.py`): NSO → C, função a função, com
  FP/SIMD em asm nativo no ARM64 (`--simd asm`) ou em C portável no x86-64 (`--simd c`), e `--dyn-base` para a
  janela de memória móvel do Android.
- **O runtime** (`runtime/`): kernel do Horizon emulado (threads, sincronização, memória, `svc`), IPC e serviços
  (`fs`, `hid`, `audren`, `vi`, ...), o carregador de NSO com relocações, a tradução **NVN → Vulkan** e o tradutor de
  shaders **Maxwell → GLSL → SPIR-V** (com as otimizações para Adreno), áudio, FFmpeg para vídeos, ImGui.
- **O Android** (`android/`): app Gradle com SDL3, janela de memória dinâmica, libadrenotools (Turnip), arena de GPU
  para drivers sem memória "placed" (Qualcomm, Mali), texturas BC decodificadas na CPU (Mali), controles de toque,
  thread de render, ritmo de quadros, geração do código do jogo no aparelho.

## O dump (medido em 2026-10-18 com `tools/botw_dump.py`)

Título `01007EF00011E000`, **v1.0.0** (build `2094F152836683A3632C6153FD84AB80` do `main`).

| Módulo | Imagem | Símbolos | Relocações | O que é | Tratamento |
|---|---|---|---|---|---|
| `main` | 35,5 MiB | 510 | 223.269 | o jogo (KingSystem/U-King, sead, Havok, vfx) | recompilado |
| `sdk` | 7,6 MiB | 14.849 | 15.275 | nnSdk | recompilado + serviços HLE |
| `subsdk0` | 2,0 MiB | 1.743 | 11.376 | `nvn.nss`, o driver NVN | HLE (`runtime/nvn`) |
| `subsdk1` | 0,2 MiB | 449 | 453 | `nnDisplay.nss` | HLE |
| `subsdk2` | 0,3 MiB | 565 | 673 | `libcurl.nss` | stub |
| `subsdk3` | 6,5 MiB | 10.231 | 18.427 | `multimedia.nss` (vídeos) | HLE com FFmpeg |
| `rtld` | — | — | — | carregador dinâmico | não usado |

`romfs`: 39.678 arquivos, **13,4 GiB**.

Para comparar, o `main` do LAS tem 29 MiB de imagem e ~105 mil relocações: o executável do BOTW é do mesmo porte
(cabe no mesmo mapa: `main` em `0x7000000000`, `sdk` em `0x7004000000`). O `nso.py` do LAS já lê todos os módulos
do BOTW sem mudanças.

## Diferenças em relação ao LAS

| # | Diferença | Onde | Gravidade |
|---|---|---|---|
| 1 | **SDK de 2017.** O LAS foi compilado com o SDK 8.2.0 (2019); o BOTW 1.0.0, com um SDK do lançamento do Switch. Comandos de IPC, versões de serviços e funções internas do nnSdk mudam. | `runtime/services/`, `ipc.c`, `hle/hle_symbols.txt` | **Alta** |
| 2 | **NVN em módulo separado.** No LAS, o NVN vem pelo `nnSdk`; no BOTW, o `main` importa direto do `nvn.nss` (`subsdk0`), numa versão antiga da API. Layouts de structs e procs podem não bater com o que `runtime/nvn` espera. | `runtime/nvn/`, `hle/nvn_boot.c`, `loader.c` | **Alta** |
| 3 | **Código do LAS ligado a endereços do LAS.** `tools/patches.py` (~60 endereços, com `assert` na instrução: o gerador para no primeiro), os hooks de jogo em `runtime/hle/` (chamber, digs, keysanity, warp, widescreen, ...), o randomizer (`tools/rando`, `third_party/LAS-Randomizer`, `runtime/rando.c`), as abas de warp e editor de save, os scripts de teste. Tudo isso sai ou fica vazio. | vários | Alta (bloqueia a Fase 2) |
| 4 | **Escala.** Mundo aberto com streaming, Havok (física, IA, tecido) em várias threads, muito mais shaders e texturas, e o jogo pode usar até ~3,2 GB de memória (o limite de um app no Switch). | CPU, GPU, cache de pipelines | Alta (desempenho) |
| 5 | **romfs de 13,4 GiB.** O app Android do LAS copia os arquivos para a pasta do app e monta uma imagem de dados: com o BOTW isso passaria de 27 GB. Ler os arquivos no lugar. | `romfs_build.c`, app Android | Média |
| 6 | **Versão.** O dump é v1.0.0. A decompilação [zeldaret/botw](https://github.com/zeldaret/botw) (nomes de funções, structs) é da v1.5.0: os endereços não batem, mas os nomes podem ser achados por assinatura. A v1.6.0 é a última; vale avaliar mudar de alvo antes de escrever hooks. | `tools/sym.py`, hooks | Média |
| 7 | **Vídeos e áudio.** `romfs/Movie` passa pelo `multimedia.nss`; o `movie.c` do LAS é ligado ao player do LAS (endereços no `main`). O caminho do áudio do jogo ainda não foi visto. | `hle/movie.c`, `services/audio.c` | Média |

## Plataforma alvo: somente Android

Em 2026-10-22 foi decidido abandonar os ports desktop. Os builds, pacotes, scripts de distribuição e backends
exclusivos de Windows, Linux e macOS foram removidos. O trabalho continua no APK Android e no companion ZeppOS.
Os relatos anteriores de desktop neste documento são históricos; não representam alvos ou pendências atuais.
O SDK/NDK Android e o Gradle são o caminho de compilação do aplicativo.

## Fases

### Fase 0: repositório e dump — **feita (2026-10-18)**

- Fork do LASRecompAndroid com o histórico (129 commits; remotos `las` e `upstream` para trazer melhorias do motor).
- `Dump/` fora do git; `tools/botw_dump.py` confere os módulos, a versão e o romfs.

### Fase 1: tirar o LAS e compilar sem o jogo — **feita (2026-10-18)**

- Saíram: o randomizer (`third_party/LAS-Randomizer`, `tools/rando`, `runtime/rando.c`), os 17 hooks de jogo do LAS
  em `runtime/hle/` (chamber, digs, doorjump, escape, flags_ext, framepace, hookshot, keysanity, miniboss, mirror,
  pets, pot_glow, qol, rescue, shopsanity, souls, warp), as abas Save Editor / Randomizer / Warp / Cheats / Gameplay /
  Cosmetics e os overlays do LAS, as opções do LAS em `settings.h`, os scripts de teste do LAS e o smoketest Linux
  (que andava pelo jogo do LAS).
- `tools/patches.py` ficou vazio, com o mecanismo e um exemplo; o gerador perdeu as variantes de patch das seeds.
- O que o runtime pergunta ao jogo ficou em `runtime/hle/game.c` (`game_in_play`, `game_level_name`: "fora de
  jogo" por enquanto). `runtime/hle/widescreen.c` virou a interface neutra que os renderers chamam (imagem 16:9 com
  barras até existir um hook de câmera do BOTW). `runtime/nvn/texpatch.c` ficou como stub (os botões no estilo do
  controle eram do LAS).
- Novo: `LARECOMP_MODS=<pasta>` com um `romfs/` que substitui / acrescenta arquivos do jogo (o mecanismo de
  sobreposição das seeds, agora para mods).
- O launcher confere o build id do BOTW v1.0.0 e acha o romfs pela pasta `Pack/` (no PC e no Android).
- Renome: `botwrecompandroid` (executável, pacote Android `com.botwrecompandroid`, `BOTWRecompAndroidActivity`, pasta
  `android/botwrecompandroid`), `botwgame` (a biblioteca do código do jogo), `botwgen` (o helper do gerador),
  `botwrecompandroid.ini`, bundle id `com.mamcjr.botwrecompandroid`. **As variáveis de ambiente continuam `LARECOMP_*`** e os
  identificadores internos também: o motor é o do LAS, e assim fica mais fácil trazer melhorias de lá.
- Verificação: o app Android (`android/botwrecompandroid`, `gradlew assembleDebug`) compila e linka com `--no-undefined`.
  Os arquivos só do macOS (`renderer.m`, `metal.m`) foram editados do mesmo jeito, mas não compilados aqui.
- O build desktop citado no plano original foi descontinuado; o alvo atual é Android.

### Fase 2: gerar o código do jogo — **gerado (2026-10-18)**

O gerador do LAS funcionou no BOTW sem mudanças (para Android: `--simd asm --dyn-base`, em `generated-android-dyn/`):

| Módulo | Funções | Arquivos C | Instruções não suportadas | Tempo |
|---|---|---|---|---|
| `sdk` | 17.689 | 98 | 0 | 6 s |
| `main` | 104.671 (1.056 tabelas de salto) | 581 | 0 | 53 s |

(Os poucos `rt_udf` no C gerado são `udf` / `brk` de verdade do código e o cabeçalho do módulo.) Os `subsdk` não são
recompilados, como no LAS: o que o `main` usa deles é HLE (abaixo).

- `android/build_gamelib.sh <exefs>` compila o `botwgame.so` com o NDK: **compilou e linkou (2026-10-18)**, 683
  arquivos em ~1 h nesta máquina (16 threads), 152 MB sem debug info. O ID (`7e833884382c3dd9`) bate com o do app, e
  os 6 símbolos que ele pede ao runtime (`rt_dispatch`, `rt_svc`, `rt_udf`, `rt_ticks`, `fnprof_hit`, `g_fnprof`)
  existem no `libmain.so`. Pronto para `android/push_game.sh`.

### Fase 3: boot até o primeiro quadro — **em andamento**

Feito sem rodar o jogo (análise estática; `tools/imports.py <exefs>` refaz):

- **Importações do `main` (483):** 431 vêm do `sdk`, o resto de HLE. Faltavam 5, agora implementadas:
  `nn::vi::SetLayerScalingMode`, `movie::Buffer::Size/Offset` (sem `const` no SDK de 2017),
  `movie::Decoder` com 2 argumentos e `movie::MediaData::FindRect`. Sobram as mesmas 12 do LAS (glslc e marcadores
  do linker, que nunca são chamados).
- **NVN:** das ~225 funções que o BOTW pede por nome, só `nvnDeviceWaitForError` faltava (implementada: espera
  100 ms e diz "sem erro"). 7 outras faltam também no LAS, que rodava sem elas.
- **Boot do SDK antigo:** o SDK de 2017 não tem `nn::init::Start`. Lá o próprio `rtld` fazia a sequência:
  `__nnDetailInitLibc0` → `nnosInitialize(handle, args)` → `__nnDetailInitLibc1` → `nndiagStartup` →
  `nninitInitializeSdkModule` → `nninitStartup` → `__nnDetailInitLibc2` → `DT_INIT` de cada módulo (main, depois sdk)
  → `nnMain` → `nninitFinalizeSdkModule` → `nnosQuickExit` (lido do `rtld` do BOTW). `runtime/main.c`
  (`rtld_start_legacy`) faz isso quando não acha `nn::init::Start`.
- **Serviços:** o SDK do BOTW abre quase os mesmos serviços que o do LAS; a mais: `nifm:u`, `prepo:a`, `csrng` (sem
  implementação: viram stubs que registram no log).
- **Símbolos que o runtime procura por nome no SDK:** todos existem no BOTW, menos `nn::init::Start` (acima).

**Primeiro teste no celular (2026-10-18, POCO F7 Ultra, driver Qualcomm):** o jogo **inicializa e apresenta
quadros** (~80–130 fps nas telas iniciais, ~300–500 draws por quadro), com áudio, entrada, saves e ~200 MB lidos do
romfs. Corrigido no caminho (todos em `runtime/`):

| Problema | Correção |
|---|---|
| `adb push` deixa os arquivos do jogo ilegíveis para o app | `android/push_game.sh` faz `chmod o+rX` |
| `fsp-srv` 203 (dados de atualização) devolvia "ok" sem objeto | responde "não encontrado" |
| Espaço de endereçamento de **36 bits** (`main.npdm` tipo 1): o SDK sorteia endereços de alias abaixo de 2 GB | `hle/address_space.c` leva os `AddressSpaceAllocator` para a região de alias do runtime |
| O jogo exige NVN **52.x (≥ 32)**; o runtime dizia 53.207 | responde 52.32 |
| Buffers de comando de 512 bytes com callback vazio | memória do host quando o jogo não dá mais |
| Áudio **REV1**: a saída tinha a seção de render info (só existe a partir da REV5) | só da REV5 em diante |
| `nfp:user` (amiibo) sem implementação | serviço mínimo: nenhum leitor |
| A thread principal gira esperando outra thread do núcleo 0 | **fatia de tempo** de 8 ms na emulação do núcleo 0 (`LARECOMP_C0_SLICE_MS`) |
| Listas de comando gravadas na memória de *comandos* (o BOTW reaproveita essa e mantém a de *controle*) | as listas vão para uma sombra (8×) da memória de **controle**, por página do endereço do jogo |
| `CopyCommands` tratado como `CallCommands` | copia de verdade |
| `nvnTextureFinalize` duas vezes no mesmo objeto (`free` inválido) | só libera texturas vivas |

Ferramentas novas de diagnóstico: `LARECOMP_WATCHDOG=<s>` (threads paradas em syscalls), `LARECOMP_CHECK_HANDLES=1`
(valida listas de comando), `android/run_boot.sh` (abre o jogo direto e grava o logcat).

Próximos problemas vistos:

- Instruções Maxwell que o tradutor de shaders não conhece: `0xc838…`/`0xc83a…` e `0xdf00…` (20 shaders nas telas
  iniciais).
- Crash em `renderer_mark_gpu_owned` (acesso a `0xa3`) depois de ~9 s.
- Ainda não vi a imagem: os testes foram com o celular em uso.

**Segunda rodada de testes (2026-10-18):**

- Corrigido: texturas finalizadas são liberadas 4 quadros depois (a thread de render ainda as usava); listas enviadas
  são copiadas no `SubmitCommands` (o jogo regrava a memória de controle logo depois, como o NVN permite);
  `CallCommands` também copia as sublistas na gravação.
- **Onde parei:** a fatia de tempo do núcleo 0 (8 ms) destrava a thread principal, mas quebra a ordem entre threads
  que o jogo assume: uma thread de trabalho grava listas no anel de memória de controle e a principal reinicia o anel
  por cima (visto com `LARECOMP_CHECK_HANDLES=1`). Quem libera o laço da principal (`main+0x8551a8`, espera a global
  `main+0x22df9e8`) é a thread t110, de prioridade 46 (menor que a da principal, 44): no Switch ela não poderia
  preemptá-la no mesmo núcleo, então deve rodar **em outro núcleo** no console. Próximo passo: ver em que núcleo cada
  thread do BOTW roda de verdade (`svcCreateThread` com núcleo -2 / `svcSetThreadCoreMask`, que o runtime ignora)
  em vez da fatia de tempo, que deve ser desligada (`LARECOMP_C0_SLICE_MS=0`) quando isso estiver certo.
- Diagnóstico novo: `LARECOMP_WATCH=<offset no main>[,<bytes>]` (quem acessa a memória, com a thread).

**Terceira rodada (2026-10-18, depois das mudanças do Astra, commit `164a1a6`, validadas):**

- O Astra implementou `svcSetThreadCoreMask` / `GetThreadCoreMask` (migração entre núcleos virtuais num ponto seguro),
  trocou o lock de tickets do núcleo 0 por uma fila e **desligou a fatia de tempo por padrão**; `MapVirtual` para o
  heap virtual de texturas de 120 MiB; contagem de referências das texturas (os descritores as mantêm vivas). Os
  testes `tools/test_android_runtime.ps1` (afinidade, ciclo de vida do NVN) passam no celular.
- **Confirmado:** o BOTW chama `SetThreadCoreMask` e põe threads nos núcleos 1 e 2 (era o que travava a thread
  principal). Com isso o jogo passa a **renderizar cenas de verdade**: até ~20.000 draws e ~7.700 passes por intervalo
  de 2 s, ~5.000 texturas, centenas de MB enviados.
- **Turnip funciona:** `files/drivers/Turnip_Gen8_V37.zip` (copiado do LAS) carrega; a memória do jogo vira memória
  da GPU (placed). Sem ele, o modo de cópia da Qualcomm é muito mais lento.
- **Onde parei — memória da GPU:** ela cresce ~0,5–0,8 GB/s até ~7 GB e o Android mata o app (lowmemorykiller). O
  número de texturas com imagem (`gpu mem`/`textures` agora no log de fps) só sobe. Não é reinicialização de objetos
  (`LARECOMP_LOG_TEXINIT` não achou nenhuma): o jogo cria mesmo milhares de texturas que, no Switch, são só
  descrições sobre a memória dos pools dele. O runtime cria uma `VkImage` própria (e cópia) para cada uma.
  **Próximo passo:** com Turnip, criar as imagens das texturas sobre a própria memória do pool do jogo (aliasing na
  memória placed) em vez de alocar e copiar; e/ou compartilhar a imagem entre texturas da mesma memória/formato.
  Também ver `[tex] type mismatch: want 6 have 3` (cubemap 64×64×6) e as instruções Maxwell `0xc838`/`0xdf00`.
- Custo a observar: `register_texture` agora espera a thread de render (`vk_async_drain`) a cada registro.

**Quarta rodada (2026-10-18, depois do commit do Gemini `1ab7837`):** o jogo chega ao Santuário da Ressurreição com
a imagem certa, a tela de título com o menu e os textos, ~35 fps, ~950 texturas e 1 GB de memória da GPU estáveis.

| Problema | Causa | Correção |
|---|---|---|
| A cena ocupava 3/4 da tela, lens flares viravam bolas azuis, Link transparente | a regra do LAS para a resolução interna (render targets 16:9 = tela) escalava a cena de 1600x900 do BOTW para 2133x1200 ao lado de alvos do mesmo tamanho sem escala | escala de render targets desligada (`LARECOMP_SCALE_TARGETS=1` volta à do LAS, para testes); a saída continua passando pelo FSR |
| Nenhum texto (menus, títulos) | os glifos de cada linha de texto ficam num buffer ~2,7 GB dentro do heap de 3 GB; o descritor do heap alcança só `maxStorageBufferRange` (2 GB no Adreno): o vertex shader lia zeros | com o heap na GPU (Turnip) e maior que esse alcance, as draws usam a tabela GMap numa janela do heap que começa nos storage buffers delas (`gwin_prepare`, como o compute já fazia); `LARECOMP_VK_NO_GWIN=1` desliga |
| Ruído colorido atrás da lista de saves | o jogo desenha as camadas da UI sobre o que o alvo tinha, sem limpar; uma VkImage nova tinha a memória que o driver reciclou | imagens de cor que podem ser render target nascem zeradas (como as de profundidade) |
| TLD4S (gather compacto) com AOFFI ou DC lia a coordenada Y do registrador errado; TMML com .x/.y trocados | decodificação do commit do Gemini | layout do yuzu (`texture_gather_swizzled.cpp`, `texture_mipmap_level.cpp`) |
| Texturas liberadas na hora no `Finalize` (sem backend e não registradas) enquanto a thread de render ainda podia usá-las; a deduplicação do Gemini dividia um `NvnTexture` entre objetos sem referência por dono (uso depois de liberar) | ciclo de vida do commit do Gemini | tabela de donos (endereço do objeto do jogo -> textura): cada `Initialize` é um dono com sua referência, só o `Finalize` dele (ou o próximo `Initialize`) a solta; cópias e segundos `Finalize` não fazem nada; texturas com a mesma descrição (memória, formato, tamanho, layout) são uma só, achadas por um hash do endereço; toda referência solta espera 4 quadros. Sem a marca `VIEW_WRAPPER_MAGIC` gravada nos objetos do jogo |

Controles (Android): o overlay de toque ganhou o analógico direito (câmera: flutuante, em qualquer ponto livre da
metade direita), o d-pad e L3 / R3, todos no editor de layout (`TOUCH_NCTL` 12 -> 19; layouts salvos continuam
valendo). Todos os controles conectados são abertos e lidos juntos (antes só o primeiro da lista do SDL). Testado no
aparelho com toques injetados pelo adb (com a opção "Depuração USB (configurações de segurança)" do MIUI ligada): os
dois analógicos (movimento e câmera) e o botão de menu funcionam. **Menu do jogo e toque:** toques rápidos não
trocavam de aba - as abas do ImGui só aceitam o clique se já estavam sob o ponteiro no quadro anterior, e um dedo
aparece e clica no mesmo quadro; os eventos de mouse que o SDL faz de um dedo agora vão ao ImGui como
`ImGuiMouseSource_TouchScreen` (como o backend SDL3 da tela inicial já fazia). Arrastar o dedo na vertical rola a
página do menu. `LARECOMP_LOG_POINTER=1` mostra os eventos de ponteiro que o menu recebe. O controle físico ainda
não foi testado (nenhum conectado).

Ferramentas de diagnóstico novas: `LARECOMP_LOG_FILE=<caminho absoluto>` (o log também num arquivo: o logcat perde
linhas num debug frame), `LARECOMP_DEBUG_TEXSTATE=1` (no debug frame: de onde vêm os texels de cada textura, view,
swizzle), `LARECOMP_DEBUG_NO_PASSES=1` (debug frame só com o log: os PNGs esperam a GPU e mudam o timing),
`LARECOMP_DEBUG_DATA` mostra os storage buffers do vertex shader; os PNGs dos passes agora são do alvo do próprio pass
e com o alfa; screenshots no Android vão para o diretório de dados (o SDL tratava caminhos relativos como assets).
Para rodar sem tocar no aparelho: `android/run_boot.sh <s> "LARECOMP_INPUT=700:A:6,..."` (A em 700 abre a lista de
saves; ~3 As levam ao santuário).

Desempenho medido no santuário (`LARECOMP_LOG_PACE=1`, `LARECOMP_GPUPROF_AT`): quadro de ~28 ms (~35 fps; o BOTW roda
a 30 fps no Switch), a thread de render ocupada ~14 ms, a GPU ~19 ms de trabalho + ~6 ms parada no início do quadro.
Na GPU: 16 ms de passes (o G-buffer de 6 alvos 1600x900: 3,7 ms) e 2,4 ms em 15 cópias de "feedback" por quadro -
o jogo liga a mesma textura em dois slots de alvo e lê alvos depois de texture barriers (passes de decal / luz), e o
runtime tira um snapshot a cada vez, quebrando o render pass. Medido sem elas (`LARECOMP_NO_FEEDBACK=1`, ler o
alvo ligado): a GPU fica mais lenta (~22,5 ms de trabalho por quadro contra ~19 ms) - ler um alvo do pass aberto tira
o Turnip da renderização em tiles (GMEM), como faria `VK_EXT_attachment_feedback_loop_layout`; as cópias ficam. E o
jogo se prende ao próprio modo de 30 fps (intervalo de apresentação 2) nos dois casos. O `upload` de ~500 MB/s do log de fps são as cópias por
submissão dos constant buffers (`guest_copy`), CPU, fora do caminho crítico da GPU.

A miniatura na lista de saves sai preta também na build atual (save novo, 2026-10-18): o jogo desenha uma textura
854x480 RGB10A2 (objeto temporário na pilha, memória em `0x3dc60a000`) num alvo 256x144 e lê esse alvo de volta
(`CopyTextureToBuffer`, que funciona: 256x144 RGBA8 para o heap). Mas nada no runtime escreve aquela memória antes:
nenhum render target, cópia ou dispatch a cobre, e ela está zerada (`LARECOMP_WATCH_TEXADDR=<hex>` registra criações,
alvos, cópias e dispatches sobre um endereço; `LARECOMP_LOG_READBACK=1` as leituras de volta). Falta achar quem a
preenche no Switch (a função do jogo que cria o objeto: `main+0xa3e5bc`).

Paredes do santuário esticadas em "cortinas" verticais e picos (os "vértices estourando"): as alturas do terreno vêm
de tiles 256x256 R16 com a flag `0x40`, preenchidos pela CPU em linhas lineares do tamanho da largura (o campo de
stride, 4096, não cabe no espaço delas); o runtime os lia como block-linear e o mapa de alturas saía em listras.
Achado por bisseção dos shaders do quadro (`LARECOMP_SKIP_FSHASH`, agora até 128) e `LARECOMP_WATCH_TEXADDR`.
Corrigido: flag `0x40` lida como linear, linhas na largura.

Luz da porta do santuário (cutscene "You are the light"): os feixes saem com uma borda horizontal dura e colunas
verticais. Reproduzir: o save com o Link no terminal da porta, A a cada ~90 quadros a partir do 2950; a luz aparece
por volta do quadro 4300 (debug frame lá). O buffer de luz já sai errado: pass 800x450 RGBA16F (partículas e malhas
dos feixes, fs `8d989710b28bd28a` e outros), que leem uma profundidade linear R16F 1600x900 (feita pelo fs
`71ac217f9871658e` a partir do depth, com snapshot de feedback) para o fade contra a geometria. A suspeita é essa
profundidade (ou a comparação dela); não confirmada. Nenhum formato sem filtro linear é amostrado (o runtime avisa
uma vez por formato se houver).
Bisseção: o fs `0363359e11684e85` (feixes) faz `TMML.B` (LOD via handle bindless, lido de `cb2`). O tradutor tinha os
opcodes trocados (0xdf58 é TMML, 0xdf60 é TMML.B) e consultava uma textura inexistente. Corrigido: o TMML.B usa a
textura do próximo TEX / TEXS com o mesmo registrador de coordenada. A imagem mudou pouco: a borda horizontal coincide
com a verga da porta (geometria na frente da luz), o que parece ser o comportamento do jogo.

TMML.B com handle em constant buffer: três fragment shaders (luz da porta, terreno e outros) só consultam o LOD, sem
amostragem depois, e falhavam na tradução ("bindless tmml"). Todo draw com eles era pulado, uns 700 por segundo
(o "skipped" do log de fps). Agora o tradutor acha o `MOV c[banco][off]` / `LDC` que carrega o registrador do handle
(`handle_cbuf`) e cria um slot bindless (`ShaderTexture.bindless_cbuf/offset`). O runtime lê o handle de 32 bits
nesse cbuf na hora do draw (`shader_texture_handle`, exec.c). Resultado: 0 draws pulados, e o santuário ficou bem menos
enevoado (paredes nítidas).

Link com as pernas "dentro do chão" (sala da porta, save 2, quadro ~2800, `bis3.sh`): NÃO resolvido. Achados:
- Com os shaders do santuário pulados (`LARECOMP_SKIP_FSHASH`), aparece o terreno de fora (chão rochoso do Planalto,
  céu, paredões), com a superfície na altura do joelho do Link. O terreno parece estar alto demais e atravessar o chão
  do santuário, mas pode ser outra coisa (perna dobrada por IK do pé, ou o Link afundado pela física). Falta uma vista
  lateral para decidir.
- Alturas do terreno: array 1024x1024x8 R16_UNORM (`f0x6`, 4 níveis, em `362ab4000` neste save), renderizado pela GPU
  camada a camada e nível a nível (views certas, `LARECOMP_WATCH_TEXADDR`). Cada camada vem de um tile 256x256 R16 com
  flag `0x40` ampliado 4x (fs `384947400`: amostra + offset de `cb3[2]`). O vs do terreno (`3dabe0800`) lê o array
  com `textureLod` (camada de `cb6[7].x`, LOD `cb6[7].y`, morph com `cb7`); a decodificação parece certa.
- Tentativa descartada: marcar os tiles `0x40` como dinâmicos (re-hash a cada uso, textures.c `upload_texture`) não
  mudou as pernas e deixou a imagem toda verde (sobrescreve conteúdo escrito pela GPU). Revertida.
- Próximos passos: comparar as alturas do array com o tile de origem (ler de volta), conferir se o tile tem
  conteúdo velho no momento do passe de alturas (frames ~1086-1222 e 1744), e tirar uma vista lateral do Link.
- Rodada 2: pulando só os 7 shaders do terreno (`013b27bb`, `178b1629`, `31fe3974`, `4ec45c9b`, `5e869bb1`,
  `8e67d14f`, `dfb4b02d`), as pernas continuam cortadas, então o terreno renderizado não é o que as cobre. Pulando
  santuário e terreno juntos, o modelo inteiro do Link aparece: joelho esquerdo levantado, perna direita esticada
  bem para baixo, quadril baixo. É pose de IK de pé (cada pé num chão diferente), não erro de skinning. A causa está
  na CPU (raycast de chão / física / IK), não na GPU.
- Pistas: SIMD e ponto flutuante rodam como asm nativo (`--simd asm`) e o FPCR é repassado (`rt_set_fpcr`), então a
  precisão bate com o hardware. Na cutscene da porta os pés ficam certos (sem IK). Correndo, as pernas parecem
  normais. Na mesma área o terreno renderizado também aparece alto demais (grama verde furando o chão perto da porta,
  e o chão rochoso na altura do joelho com o santuário escondido). Se a física do terreno também estiver alta, o pé
  esquerdo pisa nela: as duas coisas viriam dos dados de altura (lidos e descomprimidos pela CPU), ou de algum
  mecanismo do jogo que esconde o terreno dentro do santuário e que não funciona aqui.

Luz da porta, rodada 2 (debug frame 4300, `android/out/dbg48`): o buffer de luz 800x450 (pass 82) já sai com a borda
dura e as colunas verticais. O fade dos feixes (fs `36ec29700`, hash `8d989710`) compara a profundidade linear da
cena (R16F 1600x900, fs `3cb594e00`: `A/(1 - d*B) - A`) com a do fragmento. A fórmula parece certa, e a borda
superior coincide com a verga da porta (geometria na frente da luz). Os feixes amostram uma textura BC5 128x128
(`f0x4b`, view com swizzle R R R G). As colunas podem ser o desenho do jogo ou essa textura mal decodificada; não
confirmado.

Rodada 3, com referências do Switch (vídeo, porta abrindo e terminal Sheikah):
- TMML.B: o handle vem de `c[2][0x20 + 8*slot]`, a tabela de texturas ligadas do NVN (a mesma que um TEX com índice
  lê). O runtime não guarda handles no banco 2: lia 0 / 1.0, textura nula, LOD 0. Agora um handle do banco 2 vira o
  slot de textura normal. Os bancos de hardware `k` são `st.ubuf[stage][k-3]` (0 do driver, 1 constantes do shader,
  2 tabela de texturas): o caminho bindless corrigido para isso. O sol voltou a ter forma de estrela.
- Brilho Sheikah em blocos ("padrão de deformação"): o bloom renderiza nos níveis de mip 1-4 de uma textura 320x180 e
  lê de volta com um sampler; no nosso, o sampler do slot estava com LOD 0..0 (só o nível 0), então os níveis
  borrados nunca eram lidos. O jogo recicla slots do sampler pool sem esperar fence, e a thread de render (async.c)
  ainda tinha draws na fila lendo o slot. Corrigido: `register_sampler` (objects.c) espera a fila (`vk_async_drain`)
  quando o conteúdo do slot muda, como o `register_texture` já fazia. Brilho liso, igual à referência; ~29 fps,
  sem perda. A névoa da luz da porta também diminuiu bastante e a escada aparece.
- Diagnóstico novo: `LARECOMP_WATCH_TEXID=<hex>` / `LARECOMP_WATCH_SMPID=<dec>` registram os registros de um slot;
  o log do debug frame mostra o sampler (LOD, filtros, handle) de cada textura e o banco/offset dos bindless.
- Descartado: modo de profundidade 1 (`NEAR_IS_ZERO`) deixa a tela preta; o BOTW usa o padrão -1..1.
- Referência (vídeo 4K do Switch, youtu.be/7zX3D98tt-U, 3:19-4:15): o brilho laranja/azul do pedestal é liso (o nosso
  agora também); no flash da ativação (3:35) há um anel, partículas e um traço de lente fino; a luz da porta (4:05)
  tem o sol em estrela e uma faixa horizontal de feixes com borda superior reta, como a nossa (é o desenho do jogo);
  a névoa da porta no nosso ainda é maior e mais clara; a névoa do chão à esquerda é branca no Switch e esverdeada
  aqui.
- Retângulo azul da ativação: é o traço de lente (fs `36e1fc700`, hash `e97e29b6a86cf9ae`; vs `36e1fa100`, 1 quad em
  strip de 4 vértices, cantos pelo `gl_VertexIndex`), textura BC4 256x78 (um quadrante do traço, centro no canto
  inferior direito; swizzle R R R R, alpha = R) com sampler wrap 7 (clamp). A textura decodificada está certa
  (`LARECOMP_DUMP_TEXADDR`, despejo dos blocos). No nosso o `v` da textura fica constante (>= 1) em todo o quad: faixa
  uniforme com bordas duras. Wrap 7 como espelho não muda nada. Falta rastrear o `v2.y` do vs (`-r3 * cb9[48].y /
  cb9[48].w`). Os streams desse draw têm 16 bytes (um vértice) para 4 vértices: agora a leitura além do tamanho dá
  zero, como no Switch (antes lia os buffers vizinhos); não resolveu a faixa.
- Tela preta ao ativar (relato): não reproduzida em ~10 execuções roteirizadas.
- Traço da ativação e névoa da porta: com o wrap 7 lido como *mirrored repeat* (`LARECOMP_WRAP7`, teste) ficam
  iguais ao Switch (3:35, 4:05), mas a barra clara da tela de carregamento, que também usa wrap 7, passa a se
  repetir em listras: ali o 7 tem de ser clamp. Troca revertida (a9537a9). O mesmo valor não pode significar as duas
  coisas: um dos dois lados está errado no nosso (o uv do traço, 0..2 por `cb9[48] = (2, 2, 1, 1)` sobre um
  quadrante da imagem, ou o sampler que o traço realmente usa no Switch). `BindSeparateSampler(s)` nunca é chamado
  (log em cmdbuf.c), então não é um sampler separado sobrescrevendo o slot.
- Tabela de wrap do NVN: a tabela do agl (main + 0x1c21ccc, [1, 6, 7, 5, 3, 0, 3, 4]) está na ordem do próprio agl,
  não na do GX2. Lida como GX2, trocava 3 e 5 (bd75f20) e o shadow map do sol (wrap 5, clamp to border) virava
  mirror-once: sol iluminando as paredes de dentro do santuário depois da porta aberta. Voltou à tabela original
  (fa7085a): 0 / 7 clamp to edge, 1 repeat, 2 / 3 mirror clamp to edge, 4 / 5 clamp to border, 6 mirrored repeat.
- Traço / névoa / luz da porta, RESOLVIDO: o eft cria uma tabela de 16 samplers (4x4 wraps) e registra cada um pelo
  gerenciador de pool do agl, que reaproveita o slot de um sampler igual já registrado, comparando os objetos
  NVNsampler do guest. No nosso HLE o objeto só tinha o ponteiro do host e bytes velhos: o sampler mirror/mirror do
  eft "batia" com o padrão clamp do agl (índice 257) e ficava com o slot dele. Agora o `nvnSamplerInitialize` também
  grava o descritor no objeto do guest (+8). Traço da ativação, sol e feixes da porta, névoa e a luz entrando pela
  fresta da porta ficaram como no Switch. Diagnóstico: `LARECOMP_LOG_SMPREG=1` (registros), `LARECOMP_LOG_WRAP=1`,
  `LARECOMP_WATCH_HANDLE_TEX=<id>`.
- Sol iluminando o santuário por dentro depois da porta aberta: o `nvnCommandBufferSetDepthClamp` era ignorado. As
  cascatas de sombra do sol (896x896x3, `rt 0 d0x32`) recortavam a montanha acima do santuário (mais perto do sol que o
  near plane): visto do sol, o chão do corredor aparecia sem nada em cima. Com o depth clamp (66d40a9) a sala fica
  escura e com névoa, como no Switch em 3:56. Ainda aparece clara em algumas execuções (1 de 3): resta algo dependente
  de tempo. O debug frame agora salva passes só de profundidade (as cascatas, em vermelho).
- Fechamento ao recarregar (morrer -> Continuar, ou Sistema -> Carregar): contornado (2026-10-19) com um patch no
  código do jogo (`tools/patches.py`, 0x70010c1aa0). A desmontagem pula o destroy quando a heap é nula, o mesmo
  estado em que o jogo roda a sessão inteira. Testado: carregar um save pelo menu recarrega e o jogo continua;
  morrer e Continuar também (confirmado pelo usuário no celular). Para
  reconstruir rápido, edite o .c gerado correspondente em `generated-android-dyn/main/` e rode
  `android/build_gamelib.sh`: o Ninja recompila só esse arquivo (minutos). Abaixo, a causa (a falta de memória
  continua; achar qual heap está maior que no Switch fica em aberto):
  - O que falha: `act::ClusteredRenderer` (f_1010c084c, chamado de f_1010679e4) cria, da heap **OpenWorldStage**,
    a heap "clustered" (10 MiB, limitada ao maior bloco livre: recebe 9,6) e depois "act::ClusteredRenderer - DL
    Heap" (7 MiB). Essa segunda falha (sead::ExpHeap::create devolve nulo), mas o flag 0x40000 em +0xd0c é ligado
    assim mesmo.
  - Onde fecha: a sessão roda sem a heap, e a desmontagem (f_1010c1a04) chama o vtable de `this+0xc60` nulo.
    Reproduzível: `scratchpad/reload.sh` (planalto, menu, Carregar).
  - Memória: "MassRenderer - Work" pega até 20 MiB do que houver (recebe 12,6). No Switch, OpenWorldStage deve ter
    >= ~13 MiB a mais livres nesse ponto. A árvore (`LARECOMP_HEAPTREE=342000000:1500`) é RootHeap 3078 MiB (fixo) ->
    TaskMgr (resto depois de GraphicsSystem 36,3 MiB) -> RootTask -> uking::frm::System 440,4 (resto depois de
    KingSystem 2598,6, com ~30 filhas de tamanho calculado) -> GameScene -> OpenWorldStage 367,9 (resto).
  - Descartado: valores de nvnDeviceGetInteger (`LARECOMP_DEVINFO=i=v+...`: <= 0,5 MiB), máscara de núcleos (o NPDM
    diz 0-2; `LARECOMP_CORE_MASK`, sem efeito), memória total/usada (o jogo não pergunta), taxa de quadros.
  - Ferramentas: `LARECOMP_PEEK=<end>:<len>` (+ `LARECOMP_PEEK_EVERY`), `LARECOMP_CRASH_MEM=<reg>:<off>:<len>[:<deref>]`,
    `LARECOMP_WATCH_REGS`, `LARECOMP_WATCH_READ_PERIOD_US`.
- Limitador de quadros (2026-10-19): ligado por padrão. O present espera max(intervalo do jogo, teto do menu Vídeo)
  vsyncs: 30 -> 2, 60 -> 1, Auto -> 2 por enquanto. Antes os menus iam a ~200 fps e o mundo acelerava acima de 30.
  `LARECOMP_PRESENT_WAIT=0` desliga.
- Planalto (save 3, 2026-10-19 17:11): o Link aparece com as pernas cortadas no joelho na grama (o "afundando"),
  céu marrom e dessaturado; num quadro mais tarde (~2600) a cena ficou toda preta com o HUD intacto (suspeita de
  NaN na exposição automática), sem erro no log.
- Link afundando, hipótese "a CPU lê memória que a GPU escreveu" (o *reactive flushing* do Eden): descartada para
  o array de alturas do terreno (1024x1024x8 R16, f0x6, em 362ab4000). `LARECOMP_WATCH_READ_TEX=<w>x<h>x<d>` fecha as
  páginas da primeira textura com esse formato (`rt_range_watch` em dispatch.c, rearmado a cada 50 ms). Cada acesso
  é registrado uma vez por função, como leitura ou escrita (ESR WnR). Com o watch armado já no init, o jogo fechava
  (f_1043283e8, nulo + 0x10): o loader também preenche essa memória pelo kernel, e com a página fechada a leitura
  falha. Por isso arma depois de `LARECOMP_WATCH_READ_DELAY` s (padrão 60). Armado em jogo no planalto por ~60 s
  (~49 fps): 0 acessos. Na carga, f_104327900 mexe nessa memória pela CPU. Os counters do jogo são só timestamps
  (tipos 0 e 16); não há occlusion query. O pé deve vir da física / raycast (CPU), não da GPU.
  Rodada 2: o watch aceita formato e vários intervalos (`LARECOMP_WATCH_READ_TEX=0x0x8:25`, 0 = qualquer
  dimensão, só texturas >= 64 KiB). Nenhum dos arrays do terreno (alturas f0x6; 512x512x8 e 64x64x8 f0x25;
  256x256x8 f0x2a) é lido pela CPU em jogo. Intervalos com páginas de 16 KiB invadem objetos vizinhos e já
  derrubaram o jogo (nvnBufferGetAddress nulo); vigie poucos intervalos e grandes. Vista lateral no planalto
  (RSTICK_LEFT): o Link está de pé, pernas retas, afundado por igual até a canela. Não é pose de IK; o modelo
  inteiro está baixo, como na neve ou areia funda. Travar em 30 fps (`LARECOMP_PRESENT_WAIT=1`: o present espera
  o intervalo do jogo em vsyncs; sem isso o BOTW apresenta ~45-49 vezes por segundo com intervalo 2) não muda
  nada. Pista do Cemu: o afundamento no Wii U vinha do jogo usar a GPU para trabalho fora do gráfico (corrigido no
  Cemu 1.7.4). `LARECOMP_LOG_READBACK=1` não registrou nenhuma cópia textura→buffer no planalto. Pausado
  (2026-10-19): o usuário vai migrar para o update (v1.6.0) e espera que isso resolva.
- Céu marrom no planalto (2026-10-19): corrigido. O céu é desenhado a partir de uma LUT de dispersão atmosférica 3D
  (256x32x16 RGBA16F, escrita pela CPU na carga; fs `81f35db4` gera o céu 256x256, e o triângulo `38905430` o
  desenha). Lida com blocos de 4 GOBs de altura, a LUT saía embaralhada: média R:G:B 16:9:2 no lugar de azul.
  `LARECOMP_DUMP_TEX=<w>x<h>x<d>,<prefixo>` salva os bytes do guest e a versão desembaralhada de um upload. Medindo
  a suavidade dos dados em cada combinação, venceu altura 1 GOB com profundidade 16. É a regra do deko3d (que
  segue o NVN): texturas 3D só têm tile em profundidade (`pick(1.5 x depth)`). As 2D usam
  `pick(1.5 x altura em GOBs)`, arredondando para baixo até 16, enquanto o nosso arredondava a contagem para cima.
  Isso muda poucas texturas não empacotadas (`LARECOMP_LOG_BHDIFF=1` lista; `LARECOMP_TEGRA_OLD_BH=1` volta a regra
  antiga). O céu agora fica azul-claro com névoa branca, e a cena perdeu o tom sépia.
- Write-back de alvos pequenos (2026-10-19): o jogo lê na CPU (f_10120fdc4) os limites de profundidade que reduz
  na GPU até 3x3 RGBA16F (do depth 1600x900 -> 800x450 f0xa -> 192 -> ... -> 3x3, provavelmente para ajustar as
  cascatas de sombra). A memória nunca recebia esses texels. Agora alvos de cor minúsculos (1 nível, sem escala,
  até 4 KiB) são copiados no fim do pass e escritos na memória do guest quando a GPU termina
  (`LARECOMP_NO_WRITEBACK=1` desliga, `LARECOMP_LOG_WRITEBACK=1` lista). `LARECOMP_WATCH_READ_ADDR=<hex>:<len>[+...]`
  vigia leituras da CPU em bytes do guest (armado depois de `LARECOMP_WATCH_READ_DELAY`).
- Véu branco / céu amarelo no planalto: corrigido no tradutor de shaders (2026-10-19). O shader do céu (`81f35db4`,
  Bruneton) escolhe as constantes "chão" ou "céu" com `FSET.CC RZ, ...` seguido de `@P0 BRA CC.NEU`: o desvio
  depende do predicado e dos flags do FSET. O tradutor não emitia nada para o FSET em RZ e ignorava o teste de
  CC do BRA (bits 0-4), então desviava só por P0. Olhando para cima, as constantes do céu eram puladas e o shader
  rodava com valores velhos dos registradores: céu ~10x claro e amarelo, cubemap e exposição verdes, véu sobre tudo.
  - Agora FSET/ISET com `.CC` atualizam `ccz`/`ccs` (mesmo em RZ), e BRA com teste de CC (`cc_test`, o mesmo do
    CSETP) só desvia se o teste passar, inclusive no caminho estruturado (`b->cond`) e na análise de fluxo.
  - Resultado: céu azul, Montanha da Morte, castelo e serras ao longe visíveis, como no vídeo (4:41).
  - Como foi achado: `android/out/veil/simsky.py` reimplementa o shader e bate com a GPU; a LUT é o próprio
    `System/KSys/sky.skybin` (Bootup_Graphics.pack, offset 128 KiB, idêntica byte a byte); e
    `LARECOMP_SAVE_MSL=1` anota cada instrução no GLSL de `LARECOMP_DUMP_FS`.
  - Outras ferramentas: `LARECOMP_DEBUG_RAW_PASSES=1` (texels float dos passes), `LARECOMP_DEBUG_DATA` (bancos
    certos do fragment shader), `LARECOMP_LOG_UBO_UPDATE=<endereço>`, `LARECOMP_WRITEBACK_MAX=<bytes>`.
  - Ainda visível: buracos brancos nas serras distantes (o terreno distante com falhas já anotado).
- Buracos nas serras distantes (2026-10-19): são transitórios. No quadro 3000 após carregar o save do planalto, a
  serra da Montanha da Morte aparece em recortes com falhas. Nos quadros 5400 e 6600 da mesma execução está completa,
  como no Switch: o LOD de paisagem distante ainda está chegando (streaming).
  - `LARECOMP_VK_NO_CULL=1` "corrigia" só porque mostrava as faces de trás. Os testes por shader se contradiziam,
    porque mudar os primeiros quadros muda o tempo do streaming.
  - Ferramentas: `LARECOMP_VK_NO_CULL=<lista>` ou `=-<lista>` (todos exceto a lista; até 256 hashes),
    `LARECOMP_LOG_FSHASH_FROM=<quadro>` (cada fragment shader desenhado a partir dali, uma vez, com alvo e
    estado) e `LARECOMP_SHOT_AT=a+b+c` (`+` como separador no Android).
  - Em aberto: no Switch a troca de LOD não mostra buracos. Pode ser streaming mais lento aqui (E/S de arquivos) ou
    o modelo de baixo detalhe sumindo antes da hora.
- Cena preta com o HUD intacto (aberto, não reproduz). Apareceu em duas capturas no planalto: quadros 3000/4200 e
  2400-6000 com `LARECOMP_SCREENSHOT_EVERY`. Depois disso, 4 execuções seguidas não mostraram preto, nem nas
  capturas nem na tela real (`adb exec-out screencap` no mesmo momento). `LARECOMP_LOG_WB_NAN=1` vigia a exposição
  automática pelos alvos devolvidos à memória: avisa inf/NaN no 12x1 / 1x1 (R11G11B10F) e registra um quadro
  escuro (luminância 2x2 do quadro anterior < 0.02) com os 12 parâmetros. Nas 4 execuções não houve nenhum
  aviso. Pode ter sido um artefato de captura ou algo raro.
- Chão sumido abaixo do planalto (resolvido, 2026-10-20). As alturas do terreno (array 1024x1024x8 R16, 4 níveis, em
  362ab4000) ficavam incompletas: camadas 3-7 vazias no nível 0, nível 1 das camadas 0-1 vazio.
  - Como as camadas são feitas: as 0-2 vêm de tiles 256x256 R16 (fs `bdf3958d`). Cada camada 3-7 é desenhada uma
    vez a partir da anterior (fs `91fe757c`, camada em cb3[4].x, LOD em cb3[4].y); depois só faixas finas (~18 px)
    são atualizadas, como um clipmap. Os mips vêm de `35da1c5a`.
  - Causa: o primeiro draw com um shader / pipeline novo é pulado enquanto ele compila em segundo plano. O draw da
    camada 3 era o primeiro de `91fe757c`, então ela nunca era desenhada (o log `[watch] render target` mostrava as
    camadas 4-7 no nível 0, nunca a 3), e as seguintes copiavam o vazio. Com `LARECOMP_SYNC_SHADERS=1` tudo aparecia.
  - Correção: draws em alvos feitos uma vez e mantidos (arrays, cubemaps, 3D, alvos com mips) esperam o shader e o
    pipeline (`wait_built` / `rt_is_persistent` em exec.c; `LARECOMP_WAIT_PERSISTENT=0` desliga). Custo: ~210 ms
    na carga, aparece como "compile N (waited X ms)" na linha de estatísticas.
  - Corrigido no caminho: amostrar como 2D array uma textura que também é render target ligava uma textura dummy. O
    snapshot de feedback agora copia todas as camadas e níveis e é ligado com a view certa (2D ou 2D array).
- Link e NPCs afundando no chão (resolvido, 2026-10-20). Em cima de um tronco os pés ficam certos; o ancião, ao se
  levantar na cutscene, afunda igual; um Bokoblin afunda até a cintura. Varia por lugar: até a coxa na praça de pedra
  (save do usuário, reproduz sozinho no quadro 1500), até a canela na fogueira, flutuando no pé do paredão.
  - CAUSA: o ajuste dos pés (f_100d2a328 / f_100d19d40) lança 4 raios por pé (`hkpWorld::castRay`, filtro
    a1001505), que acertam o terreno na altura certa. Quando o acerto é terreno (bit 31 em +0x80 do raio), f_100d1a080
    troca a altura pela média de 3 consultas à grade de alturas da CPU (f_100f8488c -> f_100f841cc: u16 x 800/65535,
    bilinear), arredonda ao centímetro e limita a ±0.77 m da altura do raio. A grade são os 8 tiles 256x256 R16
    (render targets lineares, flag 0x40, em 365419000, 128 KiB cada; os níveis do clipmap, 7 o mais fino) que a GPU
    desenha. Eles ficavam zerados, ou eram lidos fora do lugar: o jogo endereça a linha y em y x pitch, com
    pitch = align(256 x 2, device info 57), e nós respondíamos 4096 (herdado do LAS). Com 0 de altura, o pé ia para
    o limite de baixo e a raiz do modelo descia ~0.55 m. No tronco e na árvore o acerto não é terreno: sem ajuste.
  - Correção: device info 57 (alinhamento das linhas de um render target linear) = 128, e o write-back de render
    targets lineares fica ligado por padrão, com o mesmo pitch (`nvn_linear_rt_pitch`; `LARECOMP_WB_LINEAR=0`
    desliga). As consultas dão 204.01 sob o Link (chão 204.01), e ele fica de pé sobre a grama. A grama ficou bem
    mais densa: também usa essas alturas.
  - Por que o teste antigo de `LARECOMP_WB_LINEAR=1` "não mudava nada": com pitch 4096 o jogo lia lixo (34..67 m)
    no lugar de zeros, e o limite de -0.77 m dava o mesmo afundamento. A vigilância de leitura antiga olhava outros
    endereços (os arrays do terreno), não estes tiles.
  - Ferramentas desta rodada (device.c): `LARECOMP_FIND_MTX=x,y,z,tol[,quadro]` (matrizes 3x4 com translação
    perto do ponto), `LARECOMP_FIND_VEC=x,y,z,tol[,quadro]` (+ `_QS`, `_Y`, `_WATCH`), `LARECOMP_FIND_BACK`,
    `LARECOMP_FIND_FROM`, `LARECOMP_DUMP=<endereço>:<bytes>[:quadro]` (> 4 KiB vai para `build/dump.bin`);
    exec.c: `LARECOMP_SKIP_CS=<lista>` ou `=store`. Os ganchos de diagnóstico em código gerado (raios, sphere-casts,
    IK) foram removidos; o raciocínio está acima.
  - A colisão do terreno vem de `Physics/TeraMeshRigidBody/MainField/<x>-<z>.shktmrb` (Yaz0; Havok hk_2014.2.0-r1,
    `hkpRigidBody` + `hkpBvCompressedMeshShape`; grade única de 1280 blocos, sem LOD). Os objetos (tronco) vêm de
    `Physics/StaticCompound/*.shksc`.
  - (Errado, ver CAUSA acima) Descartado: os tiles de altura 256x256 R16 (flag NVN 0x40 = render target linear, 8
    seguidos em 365419000) vivem só na GPU; a CPU só os zera na carga (f_104327900 é um memset). Devolver à memória render targets lineares
    (`LARECOMP_WB_LINEAR=1`, linha a linha), todos os alvos até 1 MiB (`LARECOMP_WRITEBACK_MAX=1048576`) ou só os
    R16 (`LARECOMP_WRITEBACK_FMT=6`) não muda a altura do Link na praça (A/B no mesmo save). O render thread
    assíncrono também não (`LARECOMP_VK_ASYNC=0`).
  - O lifter contra o Unicorn com 150000 codificações (semente 7): 449901 casos, 0 falhas.
  - A FÍSICA ESTÁ CERTA; o terreno DESENHADO é que está fora. Save do usuário (slot 1): `PlayerSavePos` = (-909.127,
    214.358, 1848.894). Altura verdadeira nesse ponto pelos `.hght` (TSCB: área 58000097A3, aresta 0.125 x 500 m,
    256x256 u16 x 800/65535; o arquivo fica em `58000097A0.hght.sstera`, SARC dentro de Yaz0): 214.351 (LOD 58) /
    214.359 (LOD 57). A praça é plana (214.32..214.36 em 1 m), e o Link aparece enterrado ~0.6 m nela.
  - O array de alturas da GPU também está certo: despejo no quadro 1500 (`LARECOMP_DEBUG_FRAME=1500`,
    `LARECOMP_DUMP_SAMPLED=1024x1024:6`). A camada 7 é a mais fina (0.244 m por texel, 250 m) e casa com o `.hght`
    (erro médio 4.7 cm num trecho de 16 m, viés 0; o Link no texel x 721, y 821: 214.359). Cada camada N-1 contém a
    N reduzida pela metade, sem viés (clipmap). Ferramentas em scratchpad: `yaz0.py`, `hght.py`, `match2.py`,
    `chain.py`.
  - (Correção: a camada 7 tem 0.122 m por texel, 125 m; casa com erro 3 mm. Os mips também estão certos.)
  - A malha de colisão decodificada offline (scratchpad `tm.py` / `query.py`: seções de 0x60 bytes a partir de
    0x2250, 723; primitivas em 0x8fb80; índices compartilhados, vértices de 32 bits (11/11/10, codec da seção) e de
    64 bits (21/21/22, domínio global); os 53792 vértices caem dentro das caixas) dá 214.352 sob o Link: igual ao
    `.hght`. A física está certa.
  - O terreno perto da câmera é o fs `31fe3974e3e8bc78` (vs `3dabb8a00`): pular os 7 shaders do terreno, ou só
    esse, mostra o Link inteiro. A altura dele é amostra x 800 (cb5[0].y), sem desvio, e a coordenada bate com o
    texel certo; o `cb3[11..13].w` é a câmera (o terreno baixa sob ela). O vs `3dabe0800` (mais longe) usa x 800 - 1.
    `LARECOMP_NO_DEPTH_BIAS=1` e `LARECOMP_NO_WRITEBACK=1` não mudam nada.
  - ACHADO: no debug frame, o osso raiz das matrizes de skinning do Link está em y 213.798, 0.554 m ABAIXO do ator
    (214.352). É a CPU (o jogo) que baixa o modelo. Hipótese: ajuste dos pés ao chão (`FootIkController`; há
    consultas de raio em lote do Havok, `TtRayCastQuery::processLeaf`) sem acertar o terreno, esticando até o limite
    (o Bokoblin, maior, afunda mais; no tronco, que é `StaticCompound`, fica certo).
  - Descartado como fonte: as cópias do `.hght` na memória (nenhuma leitura em jogo) e os alvos R16 96x96 / 192x192
    (só o runtime lê). Vigiar os buffers de constantes da GPU (3e8...) trava o jogo.
  - Ferramentas novas: `LARECOMP_FIND=<hex>[:quadro]` procura bytes na heap; `LARECOMP_FIND_WATCH=<recuo>[:<abaixo
    de>]` arma a vigilância nos acertos alinhados na mesma execução.
- Brilho Sheikah em blocos (ainda aberto): a textura do bloom (320x180, 5 níveis) é registrada uma vez, sem view
  (`LARECOMP_WATCH_TEXID=7fffffff` registra a do bloom); todas as passadas de redução / blur leem com um sampler de LOD
  0..0 (nível 0) e escrevem no nível k (view do render target); a composição final lê com LOD 1..1 (nível 1). O jogo
  só tem samplers 0..0 e 1..1 para isso. Fazemos o que o jogo pede; a diferença está em algum detalhe de amostragem.
- O lifter contra o Unicorn com as instruções do próprio BOTW, no celular (`tools/test_lift_android.py`): 20000
  codificações, 59952 casos, 0 falhas.
- Ainda falta: a luz entrando por baixo da porta enquanto ela sobe (Switch 3:48, nosso ~3745-3765): no nosso a fresta
  fica escura. Os debug frames com despejo de passes mudam o tempo do jogo e não pegaram esse momento.

Também anotado: o jogo fechou sozinho uma vez ao carregar um save (SIGSEGV no código do jogo, `f_1010c1a04+284`,
leitura do endereço 0, x0 = 0; backtrace no logcat). Não reproduzido ainda.

Ainda a ver:
o controle físico com o BOTW; a miniatura do save na lista não aparece; upload de ~500 MB/s
(`upload` no log de fps).

Falta (precisa rodar o jogo):

- Rodar: no PC (falta o MSYS2, ver Fase 1) ou no celular (`android/push_game.sh`, com o celular no adb).
- Comandos de IPC do SDK de 2017 que o runtime não conhece; a ordem de construtores entre `main` e `sdk`.
- ~~Os slots das vtables de `movie::Extractor` / `Decoder`~~: lidos das vtables do `multimedia.nss` dos dois SDKs, são
  os mesmos onde os dois têm; o BOTW abre o vídeo pelo slot 2 (`SetDataSource(const char*)`, caminhos
  `content://Movie/<nome>.webm`), agora tratado no `movie.c`.
- A revisão do renderizador de áudio (`audren:u`) do SDK de 2017: o `services/audio.c` lê o buffer de atualização no
  formato das revisões que o LAS usava; revisões antigas podem ter outro layout de vozes / memory pools.
- `romfs.bin` de 13,4 GiB + a cópia do `romfs/` (o `movie.c` e o fundo do launcher leem arquivos soltos): ~27 GB.
- Meta: a tela de carregamento / logo da Nintendo.

### Fase 4: gráficos

- Versão da API NVN do BOTW contra `runtime/nvn`; shaders do jogo pelo tradutor Maxwell.
- Meta: tela título, depois o Planalto Inicial. Cache de pipelines no disco.

### Fase 5: áudio, vídeos, saves, entrada

### Fase 6: Android

- `--simd asm --dyn-base`, NDK, `botwgame.so`; romfs lido no lugar; medir memória (aparelhos de 8 GB).
- Turnip e driver do sistema (arena de GPU), como no LAS.

### Fase 7: desempenho

Medição no planalto (2026-10-21, save do usuário perto da igreja, POCO F7 Ultra, Turnip Gen8 V37):

- 20 fps cravados. Quem escolhe é o próprio jogo: o laço do quadro do sead (f_100c4f430, `nvnWindowSetPresentInterval`
  chamado de 0x100c4f4a8) passa de 2 para 3 assim que entra no mundo. A escolha vem do tempo de quadro que ele mesmo
  mede (no Switch o jogo também cai de 30 direto para 20 quando não cabe), não dos counters de GPU: com o menu em
  60 fps os counters já saem curtos e o jogo continua em 3. Por isso o controle de FPS do menu não muda nada aqui.
- Quadro (`LARECOMP_LOG_PACE=1`): período 50 ms; o jogo ocupado ~2.6 ms; a thread de render ~19-21 ms.
- GPU (`LARECOMP_GPUPROF_AT`): ~47 ms do primeiro ao último marco, dos quais ~9 ms parada no início (esperando a
  thread de render) e ~10 ms na última linha, que vai do último pass até a próxima submissão (inclui a ociosidade
  do present; o pass em si, fs `5bf14136`, é uma cópia de uma amostra por pixel). Trabalho real ~27 ms: o G-buffer
  1600x900 de 6 alvos com ~3250 draws ~7 ms, o resto em ~230 passes pequenos e ~15 cópias de feedback (~0.2 ms cada).
- CPU (simpleperf, 8 s): nenhuma thread saturada (render ~34% de um núcleo, threads do jogo ~11% e ~8%).
- Conclusão: tradução e GPU estão em série (~20 + ~27 ms > 33 ms), por isso o jogo escolhe 20 fps. Para 30 é
  preciso sobrepor a tradução do quadro seguinte com a GPU do atual (submeter mais cedo e em pedaços), e baixar o
  custo da GPU por draw/pass (o Adreno 830 é várias vezes o Tegra X1: 27 ms indicam desperdício nosso). A resolução
  interna não muda nada no BOTW (escala de alvos desligada, ver `tex_scalable`): o jogo desenha sempre em 1600x900.
- Linha do tempo (`LARECOMP_LOG_SYNCSEQ=1`): depois do present, submissões em +4, +15, +17, +19, +21 ms; o buraco de
  ~10 ms é um pass só (o G-buffer, ~3250 draws), que a divisão por fronteira de pass (`LARECOMP_VK_CHUNK_US`, 2 ms
  no Turnip) não alcança. O jogo espera a cerca do quadro antes de apresentá-lo, então o quadro é tradução + GPU.
- `LARECOMP_VK_SPLIT_US=<us>` (novo, desligado por padrão): divide passes longos no meio, depois de um draw, quando a
  tradução passou desse tempo sem submissão (o pass reabre com LOAD). Com 3000: GPU parada no início 9 -> 2.9 ms,
  G-buffer 7 -> 8.9 ms (o custo das divisões), período médio 50 -> 45.7 ms (1500: 45.4); o jogo passa a alternar 30 e
  20 em vez de ficar em 20. Não é o bastante para 30: o trabalho da GPU continua em ~27 ms.
- Modos do Turnip (`TU_DEBUG`, com o split): automático 27 ms de trabalho, sysmem 32.5 ms, gmem 41.6 ms. O
  automático é o melhor.
- O serviço de mapa do relógio (companion) não pesa: fica desligado por padrão e só lê a memória do jogo quando o
  relógio faz uma requisição HTTP.
- Próximo: o custo da GPU - o G-buffer (~7 ms), 15 cópias de feedback (~2.5 ms) e ~220 passes pequenos (~16 ms, cada
  um com uma barreira completa no início em `begin_pass`).
- Tempo real de GPU por quadro (novo: `gpu` na linha `[pace]`, de `gpu_busy_total`): ~30 ms em 20 fps. As barreiras
  no início de cada pass custam ~1.7 ms (`LARECOMP_VK_NO_PASS_BARRIER=1`, só medida: inseguro). Por draw
  (`LARECOMP_GPUPROF_DRAWS=1`) o custo se espalha, nenhum shader domina; os shaders do G-buffer leem as constantes
  com índices fixos.
- Clocks (perfetto `power/cpu_frequency`): núcleos de desempenho com média de 1.94 GHz, prime 1.13 GHz, a ~76 °C.
  Em 20 fps a GPU trabalha ~30 de cada 50 ms, o governador vê folga e baixa o clock: o jogo não volta a caber em 30.
  Forçando o intervalo 2 (`LARECOMP_FORCE_INTERVAL=2`, só teste: o jogo acha que está em 3) a GPU cai para 23.7 ms
  por quadro e roda a 28.9 fps.
- A decisão do jogo (sead, f_100c4f430): duração do quadro anterior em this+0x240 (ticks de 19.2 MHz) -> vsyncs;
  pede floor + 1 (+ 2 se a fração passa de 0.9), nunca menos de 2. Medido: 31 a 47 ms. Correção:
  `tools/patches.py` troca essa leitura (0x7000c4f4d4) por `rt_frame_ticks` (objects.c), que com a taxa fixa no menu
  (Vídeo: Taxa de quadros 30 / 60) limita a 1.85 vsync: o próprio jogo fica no 2, com o estado coerente. Auto: a
  escolha do jogo. Com isso e `LARECOMP_VK_SPLIT_US=3000`: 30.0 fps cravados, período 33.3 ms, GPU 23.1 ms (sem o
  split: 25.5 fps, em câmera lenta). O split ficou ligado por padrão no Turnip (3 ms).
- Afundamento depois do split: conferido no save da igreja, com e sem `LARECOMP_VK_SPLIT_US=0`, o Link fica de pé
  nos dois. A captura do morro era de outro save (não reproduzido depois).
- Menu (Android): "Auto (20 / 30)" ou "30 fps", 30 por padrão; um 60 salvo vira 30 (o BOTW nunca passa de 30).
- Thread de render (simpleperf, ciclos por função; o clock varia com o calor, ~46 °C na pele): sem hash por draw
  quando o programa e as chaves se repetem (`get_program` 5% -> 1.4%), 1024 entradas no cache do hash dos fragment
  shaders, `rt_is_persistent` só quando os alvos mudam, amostra de textura de 4096 para 1024 bytes (`fnv_sample`
  10.6% -> 4.6%: sem rastreio de escrita de páginas no Android, cada checagem amostra), o relógio do split a cada
  32 draws. Render 18.6 -> 16.7 ms por quadro; GPU ~23 ms; período 33.6 ms.
- Intervalo do split (3 / 5 / 8 ms): GPU ~23 ms nos três; fica em 3 ms.
- Cópias de feedback (~15 por quadro, 0.1-0.2 ms cada): espalhadas por decais, luz e terreno. Tirá-las pede outro
  tratamento de ler um alvo enquanto se desenha nele (ler o alvo direto já foi mais lento: sai da GMEM).
- Cópias de feedback: um alvo só lido (sem escrita deste draw nem de um anterior no pass, `draw_writes` /
  `pass_written`) é amostrado direto, sem cópia (o Vulkan permite: não há laço sem escrita). Cópias ~15 -> ~11 por
  quadro (`fbcopy N (read M)` na linha de estatísticas), GPU 23.4 -> 22.5 ms, render 17.4 -> 16.1 ms.
  `LARECOMP_FEEDBACK_ALWAYS=1` volta ao antigo.
- A GPU paga por draw, não por pixel. O modo portátil (`LARECOMP_HANDHELD=1`, ideia do nxnative/TOTK: o jogo
  desenha 1280x720 em vez de 1600x900) deixa a GPU nos mesmos ~23 ms (G-buffer ~7.5 ms com 3000 draws a 720p).
  Compressão e early-Z não pesam: `TU_DEBUG=noubwc` +1 ms, `TU_DEBUG=nolrz` igual; render targets sem
  MUTABLE_FORMAT (`LARECOMP_VK_NO_MUTABLE=1`, só medida) igual.
- Driver da Qualcomm (`LARECOMP_VK_DRIVER=none`): com o cache quente, render 33.7 ms, GPU 24 ms, 13-15 fps e draws
  pulados (o primeiro run frio mostrou GPU 17.6 ms, mas compilando). O Turnip continua melhor.
- Driver da Qualcomm (pedido do usuário: base para 60 fps): a imagem sai quebrada (sem luz, o Link em silhueta
  preta) e ~200 draws pulados a cada 2 s. Causa principal: sem `VK_EXT_map_memory_placed` nem
  `VK_EXT_external_memory_host` o heap do jogo não vira memória da GPU; o modo de cópia (`gcopy`) copia ~4 GB a
  cada 2 s (render ~34 ms) e não sincroniza o que um compute escreve com o que os fragment shaders leem (a lista de
  luzes). Mais 2 compute pipelines falham (`-13`). Extensões do driver (`LARECOMP_VK_LIST_EXT=1`):
  `VK_KHR_external_memory_fd`, `VK_ANDROID_external_memory_android_hardware_buffer`, `VK_KHR_map_memory2`,
  `VK_EXT_descriptor_buffer`, `VK_KHR_push_descriptor`, sem placed / host.
  Tentado e desfeito: (1) `vkMapMemory` e `mremap` para o HEAP_BASE - o kernel recusa mover o mapeamento do KGSL
  (EINVAL; e o mremap com MREMAP_FIXED desmapeia o destino antes de recusar); (2) memória exportável
  `OPAQUE_FD` e `mmap` do fd no HEAP_BASE - `VK_ERROR_INVALID_EXTERNAL_HANDLE`; (3) um AHardwareBuffer BLOB
  (fd do native handle mapeado no HEAP_BASE e importado no Vulkan) - o gralloc recusa qualquer BLOB neste processo,
  até 16 MB, com qualquer combinação de usos (-129; -22 com GPU_SAMPLED_IMAGE). As três vias foram desfeitas. Sem
  heap compartilhado, o driver da Qualcomm fica no modo de cópia: sem luz e ~2x mais lento na thread de render.
- **Driver da Qualcomm: imagem corrigida (2026-10-21).** A falta de luz não vinha do modo de cópia: o Turnip no modo
  de cópia (`LARECOMP_VK_NO_PLACED=1`) ilumina normalmente. O G-buffer liga albedo e normais em dois slots de cor cada
  (1/2, 3/4) e escreve só um de cada par; o driver da Qualcomm gravava por último a cópia de tile do slot mascarado
  (conteúdo antigo). `begin_pass` agora deixa ligado só o slot escrito. Também: leituras sem comparação usam um
  sampler sem comparação (`vk_sampler_plain`). Os 2 compute pipelines (`-13`) foram corrigidos pelo Astra (busca
  linear no GMap). Detalhes em [ANALISE-DRIVER-PADRAO-2026-10-21.md](ANALISE-DRIVER-PADRAO-2026-10-21.md).
- **Driver da Qualcomm, desempenho (2026-10-21):** rastreio de escrita de páginas no Android (mprotect + SIGSEGV) liga o
  cache de vértices no modo de cópia: ~90 MB de vértices/índices copiados por quadro viram ~30 KB; thread de render
  ~37 -> ~18,5 ms. O fps fica em ~15-17: tradução e GPU em série (o jogo espera a fence do quadro recém-enviado).
- Folga hoje: render ~16 ms livres, GPU ~10 ms. Para 60 fps seria preciso GPU <= ~14 ms (~40% menos) e um patch
  no jogo (o sead nunca escolhe menos de 2 vsyncs, e a lógica do jogo conta com 30).

- Threads do Havok e dos jobs do jogo nos núcleos (o LAS precisou emular o núcleo 0 do Switch), thread de render,
  resolução interna, ritmo de quadros (o jogo roda a 30 fps no Switch).

### Fase 8: distribuição

- Distribuição do APK Android, sem nada do jogo.

## Fila de funcionalidades

Pedidos ainda não começados, o mais recente no fim.

- **Arquivos do jogo no cartão SD** (pedido no Reddit, 2026-10-24: o BOTW ocupa o armazenamento interno inteiro de
  muita gente). Hoje a configuração copia o jogo para a pasta de dados do app no armazenamento interno
  (`/sdcard/Android/data/com.botwrecomp.android/files`), e os saves ficam lá; escolher o SD como origem só importa de
  lá. Caminho provável:
  - `getExternalFilesDirs(null)` dá a pasta do app em cada volume, inclusive no SD removível, sem permissão extra. A
    configuração escolheria o volume e passaria essa raiz de dados ao runtime (`LARECOMP_DATA_ROOT`, que
    `android/run_ver.sh` já usa).
  - O `romfs.bin` tem ~15 GB: SD em FAT32 (no máximo 4 GB por arquivo) não serve, só exFAT. Conferir o espaço e o
    sistema de arquivos antes de copiar.
  - O código do jogo (`botwgame.so`) continua indo para o armazenamento interno (o Android não carrega código do SD);
    só os dados mudam de lugar.
  - SD é mais lento que UFS, e o romfs é lido no lugar: medir o carregamento e se há travadas ao andar pelo mundo.
  - Desinstalar o app apaga essa pasta no SD também. Com o cartão fora, o app deve avisar, em vez de dar os arquivos
    do jogo como faltando.
  - Quem pediu lembrou que um dos projetos LAS faz isso. O LASRecompAndroid não faz; falta conferir o
    LASRecomp-Android-Plus (Nycolazs).
- **Controles de toque estilo FPS** (pedido no Reddit, 2026-10-24): movimento e câmera pelas duas metades da tela,
  para deixar a tela mais limpa. Hoje (`runtime/ui/touch_controls.mm`) os dois analógicos já flutuam: o esquerdo nasce
  onde o dedo toca em `x < 45%` da largura e abaixo de 30% da altura, e o direito (câmera) em `x > 50%`, abaixo de 25%,
  fora dos botões. Mas os dois ficam desenhados no lugar de descanso mesmo sem toque, e a câmera é um analógico: o dedo
  precisa ficar segurando fora do centro. Falta:
  - **Câmera por arrasto:** a velocidade do dedo na metade direita vira a inclinação do analógico direito a cada
    quadro (volta a zero quando o dedo para), com sensibilidade ajustável. A câmera do BOTW tem aceleração própria no
    analógico: testar se o arrasto não fica com atraso. Para mirar (arco, câmera da Pedra Sheikah) o jogo também lê o
    giroscópio quando os controles de movimento estão ligados: o arrasto como giroscópio seria mais preciso ali.
  - **Analógicos invisíveis sem toque:** desenhar cada um só enquanto um dedo o segura (uma opção na aba Controles:
    sempre visíveis / só ao tocar).
  - **Áreas maiores:** cada metade inteira da tela, com os botões ainda na frente.

## Regras

- Nada da Nintendo no repositório: nem o dump, nem código gerado, nem assets (ver `CONTRIBUTING.md`).
- Mudanças no motor que não dependem do BOTW devem ficar fáceis de levar de volta ao LAS (e vice-versa).
