# Plano: BOTWRecompAndroid

Recompilação estática de *The Legend of Zelda: Breath of the Wild* (Nintendo Switch, **v1.0.0**) para Android, a partir do motor do [LAS Recomp](https://github.com/peiikai/LASRecomp) e do seu
porte para Android ([LASRecompAndroid](https://github.com/mamcjr/LASRecompAndroid)). Como no LAS, não é emulação:
o código ARM64 do jogo vira C, compilado para a máquina, e o NVN (a API gráfica do Switch) é traduzido para Vulkan.

O plano do porte do LAS para Android, com as medições e decisões dele, está em
[docs/las/ANDROID-PORT.md](las/ANDROID-PORT.md). O README original do LAS está em [docs/las/](las/).

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
- **Do TOTKRecompAndroid (2026-10-26), duas correções do motor:**
  - **Memória local dos shaders do tamanho que eles usam.** O tradutor declarava `uint lmem[256]` e zerava as 256
    palavras em toda invocação, mesmo num shader que só guarda um registrador lá (`STL` / `LDL` em `[0x0]`, spill do
    compilador da Nintendo): 1 KB por thread vai para a memória privada, e o laço escreve 256 vezes por pixel. No
    TOTK um draw de material levava 95-102 ms de GPU por isso. Agora o vetor tem o tamanho que os acessos pedem (os
    de endereço constante: as palavras que alcançam; com endereço em registrador: o do SPH, ou 256 sem SPH), uma
    potência de 2 (`lmask_`). Fuzz GLSL (4.000 programas, mesma semente): 3.051 compilados, as mesmas 31 falhas de
    `textureGather` que o tradutor anterior dá; o teste de operações do TOTK dá as mesmas 16 falhas que antes (as
    correções que só o TOTK tem, como o deslocamento com sinal do `LDC`).
  - **`gen.py`: a rede de segurança das tabelas de salto só nos inícios de bloco.** Toda função com tabela ganhava um
    switch com um case por instrução (caso a emulação da tabela perdesse um alvo): uma aresta para cada instrução.
    Agora só os alvos de desvio, os das tabelas e cada instrução depois de um salto, retorno ou trap; uma função com
    tabela não resolvida mantém todas, como antes. 1.8.2, os mesmos 603 arquivos: **202 -> 74 min de CPU**, o mais
    lento 13,3 -> 5,2 min, a compilação inteira no PC em 7 min; nenhum arquivo acima de 15.000 rótulos (-O0). Também:
    a limpeza dos arquivos de uma geração anterior só reconhecia nomes de 3 dígitos.
  - Muda o id do código do jogo: o de cada versão compila de novo. **No celular (POCO F7 Ultra, Turnip, 2026-10-26):**
    o 0.1.11-beta com as duas (sem nada mais) recompilou o código da 1.8.2 pela tela de arquivos do jogo em **12 min**
    (717 s, até 8 compiladores, tudo a -O1, nenhum arquivo a -O0) e o jogo carregou uma cópia do save no Grande
    Platô: 150 s andando pelo campo a ~30 fps, a imagem certa, nenhum erro.

- Threads do Havok e dos jobs do jogo nos núcleos (o LAS precisou emular o núcleo 0 do Switch), thread de render,
  resolução interna, ritmo de quadros (o jogo roda a 30 fps no Switch).

### Fase 8: distribuição

- Distribuição do APK Android, sem nada do jogo.

## A fazer (atualizado em 2026-10-28)

A lista corrente; a fila abaixo guarda o histórico de cada item.

**Próxima versão (0.2.3, versionCode >= 23; lançar só com o OK do usuário).** No `main`, não lançados: c2a2320
(write tracking), ba93baa (setup nativo sem build), 0d3731c (sync lag depois de cada carregamento), 056a9a0 (arena da
Mali coerente), fa56eb3 + f1d15ba (mods que adicionam arquivos, mods do DLC, arquivos do jogo e mods no lugar),
2901800 (SavageWind: upsample, IADD3), 1ff912a (gathers a 1/256 de texel: os pontos na grama), 7e3c35d (saves com
arquivos de outro dono, #77), 54e90d8 (sem pontas nas partículas no hit-stop), dd01c2a + 325da58 (aspecto livre: menus,
Game Over, loading; o "ME"), 833cb5d (memória do jogo
na GPU só com 7 GB ou mais de RAM, #83), 05cb1b6 (giroscópio do celular com controle sem giroscópio, #78).
- Antes: testar no POCO os mods "no lugar" (nunca testados) e ver se o resto da tela continua igual com os gathers
  arredondados (sombras, água, reflexos). Os shaders compilam de novo uma vez (o GLSL dos gathers mudou): dizer nas notas.
- Depois: postar as respostas de `docs/RESPOSTAS-ISSUES-2026-10-28.md` (#43, #70/#72/#75, #71/#67/#62/#54, #74, #65)
  e avisar #21 e #80 dos pontos na grama.

**Gráficos (lista do usuário, 2026-10-28; recompilado e nativo, salvo indicação):**
- ~~A malha de pontos acima de 50% de resolução~~ (1ff912a). As linhas pretas que correm no chão à frente do Link são
  provavelmente o mesmo defeito: conferir.
- ~~Vértices tremendo no impacto de um golpe~~: as pontas na fumaça da Montanha da Morte, na névoa e na fumaça dos
  monstros no hit-stop (corrigido, ver a fila).
- ~~Tela de Game Over mostrando só "ME"~~ (325da58): os glifos do texto são lidos pelo vertex shader de um buffer
  que o jogo reescreve a cada quadro; com o sync lag a GPU lia o do quadro seguinte. Agora copiados ao gravar o draw.
- FPS mais alto e mais estável.
- Texturas piscando (raro).
- ~~Aspecto livre: recorte no menu, bordas da tela de morte, loading que encolhia e esticava~~ (dd01c2a, 325da58;
  testado no POCO). Falta conferir a tela de título e os vídeos com essas mudanças.
- Aspecto livre, troca de páginas do menu (Inventário ↔ Sistema): 0f49d51 recorta páginas e texto na parte 16:9
  (deixado assim por enquanto, a pedido do usuário). A última versão (as peças do escurecimento sem recorte) não foi
  conferida em jogo; numa tela mais larga que 16:9 a página ou aparece cortada na borda 16:9 durante a troca, ou mostra
  a página vizinha nas laterais. Ferramentas: `LARECOMP_LOG_MENUCLIP`, `LARECOMP_LOG_PANEFIX`.
- Tela preta com o driver padrão da Qualcomm.
- Menus a 60 fps, como as cutscenes.
- Só no nativo: o Link pisca ao levar o primeiro golpe.
- A grama que só carrega de perto: devolver as máscaras de densidade (512x512 e 256x256) à CPU.

**Issues novas (2026-10-28):**
- **#76** (Snapdragon 8+ Gen 1; um segundo usuário com o mesmo aparelho): tela preta com o HUD a 29 fps, e o app
  fecha. O log anexado é da sessão anterior (só a tela inicial). Ele usa o Turnip v27 "A8xx" num Adreno 730, que o
  driver identifica como "725". Pedir o `botwrecomp-log-previous.txt` logo depois do crash, e um teste com o driver do
  sistema.
- **#77** (Turnip Gen8 V37, 0.2.2): dois problemas.
  - `svcBreak(reason=0)` em 0x10437eba4 depois de importar um save editado: o jogo aborta sozinho, provavelmente por
    dados inconsistentes no save.
  - "Importar saves" falha com `EACCES` em `v160/save/save_0/2/game_data.sav`, um arquivo da pasta do próprio app que
    ele não consegue abrir: o dono é outro (copiado por outro app, root, ou uma instalação anterior). O `svcBreak` é
    o mesmo problema: o jogo não abre o próprio save. **Corrigido** em 7e3c35d (testado no POCO com uma pasta de
    teste): ao montar o save, arquivos legíveis de outro dono viram uma cópia do app (`save_0.before-own` guarda a
    anterior); ilegíveis param com uma mensagem que diz o arquivo e manda usar Importar saves (que já troca a pasta
    inteira); o Exportar explica o mesmo. Resposta no rascunho de 28/10.
- **#78**: o giroscópio não funciona com um controle USB-C encaixado no celular (GameSir X5 Lite, que não tem
  giroscópio próprio, ao que parece). Usar o giroscópio do celular quando o controle não tiver um.
  Feito em 05cb1b6 (sem controle assim para testar): os sensores do controle só substituem os do celular depois que o
  giroscópio dele se move; um controle que diz ter giroscópio e manda zeros segurava os do celular. Perguntar se a
  opção Controles de movimento está ligada.
- **#79** (Samsung, Android 16, tela 2340x1080): as Configurações da tela inicial ficam pretas, com só um canto do
  painel aparecendo embaixo à direita: o layout sai da tela. Pedir o modelo e se é dobrável, DeX ou janela.
- **#80** (AYN Odin 3, Adreno 830, 1.8.2): as linhas pretas na grama são os pontos corrigidos em 1ff912a. Responder
  com a 0.2.3.
- **#81** (Dimensity 8400, Mali-G720): sem descrição. Pedir o log; provavelmente a arena da Mali (056a9a0).
- **#82**: pedido de escolher um `.nsp`/`.xci` e extrair exefs e romfs no celular (com as `prod.keys` do usuário), como
  o LASRecomp Android Plus. Decisão do usuário (chaves e o que entra no app).
- **#70** (Adreno 650): abre com "Memória do jogo na GPU" ligada, mas a 7-10 fps na caverna.
- **#83** (Mi 9T Pro, Snapdragon 855, Adreno 640, 6 GB, 1.6.0, 0.2.2): no modo nativo chega ao título, mas cai quando
  o mapa termina de carregar (save) ou pouco antes de o Link acordar (jogo novo); o log diz "signaled (reason 2,
  status 9)" com ~3,9 GB de RAM: o Android matou o app por memória, com muitos "shader still building" antes. No
  recompilado cai na hora. O log anexado: Turnip v26 R5, `guest heap 3120 MB is GPU memory` - a "Memória do jogo na
  GPU" ligada à mão (o Automático só liga no Adreno 8xx), 3,1 GB de memória da GPU reservados de uma vez num celular
  de 6 GB; a sessão anterior terminou com 3.664 MB de PSS. Feito em 833cb5d: abaixo de ~7 GB de RAM a opção é ignorada
  e o log diz por quê (não testado num celular assim). Falta responder, pedindo para desligá-la até a 0.2.3.
- **#85** (Helio G100, Mali-G57 MC2, 8 GB, 1.9.0, 0.2.2; o mesmo no comentário novo do #1): com o driver
  PanVK-FristOneRR-V.3.5 (sem ele, fecha ao dar Play) chega à primeira cutscene e aparece uma mensagem de erro.
  A screenshot: `vkWaitSemaphores: -4 (device lost)` - o trabalho da GPU travou ou falhou no PanVK, na primeira
  cutscene (vídeo ou os primeiros shaders do mundo). Sem um Mali para testar; pedir o log
  (`botwrecomp-log-previous.txt`) e se com outro PanVK passa.
- **Tela de Game Over com só "ME"** (usuário, 2026-10-28): numa segunda morte o texto saiu inteiro; a gravação só
  pegou o começo da animação. Intermitente: falta saber se ficou parado assim, e a quantos fps.

- **#52** (Infinix GT 30 Pro, Mali-G615; dois usuários com o mesmo celular, 1.8.2): o build para no `main_012.c` com
  `error: checking cache failed: file_open` do zig - o cache dele (`.gamebuild/zig-cache`, na pasta de dados) não abre
  um arquivo. A mensagem "2 GB livres" era a genérica. Respondido pedindo o log da 0.2.3.
- Todas as issues abertas respondidas em 28/10 (segunda leva em `docs/RESPOSTAS-ISSUES-2026-10-28.md`).

**Triagem de 2026-10-30 (depois da 0.2.4; logs baixados e conferidos):**
- **Confirmadas corrigidas na 0.2.4:** build #92 (fechada pelo autor), desempenho Mali de volta (#99 G615 13-19 fps,
  #54 "performance fixed, no crashes or glitches", #89 24-30 fps, #93 "working like the previous version", #44 texto
  continua certo na G2-Ultra), save de outro app #77 ("now fine"), pontos na grama #80 (fechada).
- **Regressão da 0.2.4 - giroscópio (#78 crash ao mirar + "ainda não resolve", #115 S24 Ultra "gyro broken"):** o
  critério de 05cb1b6/3976ab9 ficou rígido demais: exigir mag > 0.05 por 250 ms contínuos nunca engata uma mira lenta
  (controles reais em repouso mandam ruído ~0.001-0.02, nunca zero exato). Trocar para: engata com N leituras
  consecutivas != 0 exato (o GameSir manda zero exato), volta ao celular com 2 s de zero exato. O crash do #78 não tem
  log (anexou o da sessão seguinte): pedir o botwrecomp-log-previous.txt.
- **#87 validado, causa achada:** `setup failed: romfs/ has 7094 files ... System/Version.txt` ausente - 7094 é o
  romfs do DLC: o find() do Activity/launcher achou o romfs do DLC (tem Pack/) antes do romfs do jogo na pasta do SD.
  Corrigir: um romfs só vale com System/Version.txt (AocVersion.txt = DLC), e seguir procurando.
- **Família dos vídeos (prioridade):** #113 (Adreno 825, 1.8.2 no lugar): `[movie] cannot open .../romfs/Movie/
  Demo152_0.webm` e SIGSEGV 0x189 logo depois - falha de open sem tratamento derruba o jogo (cutscene do Domínio
  Zora); conferir por que não abriu (dump sem o arquivo?). #93 (Mali G720): log termina exatamente em "first frame
  decoded" - crash na primeira moldura do vídeo na Mali. #86/#107 (Xclipse 530): primeira moldura decodificada e o
  jogo congela em tela preta na torre. #22 (Adreno 840): nem decodifica. Quatro sintomas no mesmo caminho do vídeo.
- **Crashes depois de minutos em Mali (cópia, native e recompilado): hipótese nova.** #54 (`0x80000042480a38`, odd
  write faults 6), #89 (host pc em f_1013f9e40, 0x14c, odd 8), #44 (G2-Ultra 16 GB: SIGSEGV code 2 **write em página
  com estado 2 = gravável**, odd 9, e no mesmo instante `Scudo: internal map failure (Out of memory)` + malloc NULL
  com RAM livre). Tudo aponta para **esgotamento de VMAs (vm.max_map_count, 65530)**: o write tracking alterna
  RO/RW página a página e fragmenta o heap em dezenas de milhares de mappings; mprotect e o malloc do scudo passam a
  falhar com ENOMEM. A Adreno do POCO não sofre porque o heap fica na GPU (sem tracking). Verificar: contar linhas de
  /proc/self/maps ao longo de uma sessão com LARECOMP_VK_NO_PLACED=1; mitigação: rearmar RO em faixas contíguas
  (uma mprotect por faixa, os VMAs se fundem) e/ou parar o tracking perto do limite.
- **#112 (3122380113): análise excelente e plausível** - árvores distantes pretas com resolução dinâmica != 1x: o
  passe de material lê o ID (7) de um alvo escalado fora do centro do texel, `trunc(x*255)` dá 6 e o passe de luz pula
  o pixel. Mesma família do gather snap (1ff912a). Corrigir: arredondar (round) a conversão UNORM8->int no translate,
  ou amostrar no centro do texel nesse padrão. Pode explicar também as folhas escuras (botw-dark-leaves).
- **#111 (Adreno 735 Turnip 24.3, 1.0.0 nativo):** null deref no código do jogo ~50 s depois do boot, logo após um
  gmiss learn; 6-8 fps. Pedir teste no recompilado e na 1.6.0+; a PR #103 (origin) muda o heap Auto nos 7xx renomeados.
- **#101 (Mi 9T Pro):** logs anexados são da sessão seguinte (15 linhas); o recompilado "crasha antes dos botões".
  Pedir o log-previous logo após o crash. Nativo 5-11 fps = CPU-bound (como #108, #70).
- **#108 (Adreno 630, SD845):** nativo roda, 5-7 fps CPU-bound; e bug separado real: `FATAL: vkCreateSwapchainKHR
  -1000000001 (surface lost)` ao voltar do background - recriar a surface em vez de FATAL. PR #96 (AAudio resume)
  aberta do mesmo autor.
- **Outros:** #100 (crash após 5 min, log sem o momento do crash; `vsync timer late... host throttling` antes), #104
  (SD888 crash no início = #58), #105 (MTK 9400: congela na floresta, manchas roxas; logs não mostram o congelamento),
  #109 (G720: flicker colorido depois de mexer nas configurações em jogo), #110/#116 (pedidos de otimização, Adreno
  610 e Mali fracas), #114 (Poco F7 8s Gen4: stutter após 5 min com v38+SGSR, térmico?), #102 (APK "corrompido" no
  HyperOS: download/instalador), #61 (G68: 0.2.4 13 fps < 0.2.2 17 fps; bomba azul e Stasis congelam; corpo do Link
  parcialmente invisível), #85 (G57 MC2 segue 2-7 fps), #9 (G68: OOM na compilação de shaders), #44 (60 fps cai para
  ~30 depois de um tempo: investigar junto com o pacer).

**Triagem de 2026-10-30, tarde:**
- **#54 (Mali G720, 0.2.4, 1.9.0 recompilado, romfs no lugar + DLC):** desempenho melhorou ("ainda pior que a 0.2.2"),
  mas agora **crasha sempre ao pegar o parapente** (evento do velho no Platô) - progressão travada. O log-previous
  anexado não é o do crash: cobre só 12 s de boot (termina em 22.768 sem `*** SIGSEGV`, nenhuma linha do handler).
  Pedir o log-previous logo depois do crash no parapente e, se possível, o save de antes do evento (repro em casa:
  Mali não temos, mas dá para ver se a Adreno com LARECOMP_VK_NO_PLACED=1 cai no mesmo ponto). Encaixa nos "crashes
  depois de minutos em Mali" (hipótese dos VMAs, acima).
- **#117 (Retroid Pocket 5 / SD865 Adreno 650, 0.2.2, 1.8.2 recompilado, Turnip 24.3 R9v2):** SIGSEGV logo depois de
  JOGAR - o log acaba em `transient ring`, antes do `[gamelib]` (carga do botwgame.so), sem linhas do handler; o run
  anterior também terminou com status 11. Não é o #58 (esse cai mais tarde, lendo o save). 0.2.2 é anterior às
  correções do build (#52/#92) e do botwgame.so com UBSan (#56/#60): pedir 0.2.4 (refaz o código do jogo se o id não
  bater) e, se continuar, o log-previous + testar com o driver do sistema.
- **#116 (Mali G610, 9-13 fps):** mais um pedido de desempenho Mali - mesma fila de #110/#61/#85.
- **Achado nosso, sem issue - bokoblins na 1.0.0 (nativo e recompilado; a 1.9.0 não tem):** uma linha de sombra fina
  sai do bokoblin vivo e vai ao infinito; ao morrer, o corpo (já escuro do efeito de morte) e a sombra esticam numa
  cunha preta até um ponto fixo no horizonte; ao apanhar parecem afundar no chão. Um vértice vai para a origem do
  mundo no passe de sombra (896x896, vs 3da039b00/3da258600, fs d6cd7c058b093cb7). Paleta (71 ossos), índices (<= 70)
  e pesos conferidos no debug frame: corretos. Também um crash em combate na 1.0.0 nativa: vtable =
  0xbf80000000000000 (floats 0.0/-1.0) num laço de chamadas virtuais (main 0xad63cc).
  Descartado no mesmo dia (cada um com o usuário lutando): placed heap (LARECOMP_VK_NO_PLACED=1), modo nativo (o
  recompilado tem o bug; a 1.9.0 não, nos dois modos), compute que grava na memória (LARECOMP_SKIP_CS=store), timing
  (LARECOMP_VK_ASYNC=0 + LARECOMP_SYNC_ALL=1), o shader de skinning (3 ossos só onde o 4º peso é enchimento: pesos
  1-3 somam 255 em todas as malhas; paletas sem matriz anômala), as HLE só da 1.0.0 (GetNpadState escreve 0x28 =
  o que o jogo reserva; six-axis 16 x 0x60 = 0x600 do jogo). Desligando vertex shaders ao vivo (build/skipvs.txt,
  ferramenta nova em exec.c): sem nenhuma sombra a linha continua - não é do passe de sombra.
  **Crash reproduzível (2x, 11:33 e 12:12, mesma pilha):** main 0xb56690 percorre as ligações de uniform blocks de um
  programa (flags, máscara de estágio, ponteiro opcional, objeto) e chama 0xb0dee8 -> `nvnBufferGetAddress(obj+0x48)`
  com obj = 0x0001000600000001 (dois inteiros pequenos no lugar do ponteiro). Mesma assinatura do vtable: dados do jogo
  sobrescritos. **Estacionado por decisão do usuário (2026-10-30):** recomendar a 1.8.2 ou a 1.9.0 a quem
  encontrar (combate na 1.0.0 com linhas/riscos pretos ou crash). Suspeita atual: corrupção de memória específica da 1.0.0 (HLE do NVN antigo escrevendo além do
  tamanho do objeto da versão 1.0.0? tamanhos dos objetos NVN mudaram entre SDKs) - a linha seria um sintoma
  visual da mesma corrupção (um dado de desenho trocado). Próximo: achar quem escreve nesse array (watchpoint de
  escrita no endereço com LARECOMP_WATCH / mprotect) e conferir os tamanhos dos objetos NVN da 1.0.0 contra o que as
  nossas HLE escrevem.

**Triagem de 2026-10-30, noite (depois da 0.2.5; logs baixados e conferidos):**
- **#126 (Xclipse 530, 0.2.5, 1.0.0):** os riscos pretos nos inimigos mortos, no Link nos santuários e no monge = o
  bug estacionado da 1.0.0 (acima). Respondido: só na 1.0.0, usar a 1.8.2 ou a 1.9.0. Bom sinal no mesmo relato: a
  0.2.5 destravou o vídeo da torre no Xclipse e foi de 10-15 para 20-30 fps (720p + dinâmica).
- **Import do heap na Mali - prioridade (#121, #118, junto com o #44 da G2-Ultra):** os drivers Mali novos expõem
  VK_EXT_external_memory_host e o heap de 3120 MB é importado. #121 (Mali-G1-Ultra, driver r54p1, 0.2.4, 1.9.0
  nativo): sem texto em nenhum menu e blocos cinza na arte do título - igual ao #44; com
  `LARECOMP_VK_HOST_IMPORT=0,LARECOMP_VK_NO_GRES=1` tudo certo e a RAM cai de 4018 para ~1,8 GB. #118 (Nothing Phone
  2a, Mali-G610 MC4, r56p0, 0.2.4, 1.8.2 nativo): importou os 3120 MB, a corrida anterior foi morta por memória baixa
  (rss 4470 MB) e o Android inteiro travou. As Mali sem a extensão (G720/G925 r49p1, G57 r32) já vão pelas cópias.
  Corrigir: import desligado (e NO_GRES) em toda Mali por padrão (vendor 0x13B5, como já é o Xclipse e a G2-Ultra);
  e "Memória do jogo na GPU" = Desligado (gpu_heap 0) desligar também o import - hoje só vale para o heap placed do
  Turnip, então a opção não ajuda na Mali (o #121 tentou gpu_heap = 0 no ini).
- **#122 (ibka512, fontes):** "Importar fontes" nunca funciona no Android - o diálogo do SDL devolve `content://`, o
  nome comparado é o document id e o `fopen` não abre a URI (`launcher.mm` import_fonts). Corrigir: nome via
  `OpenableColumns.DISPLAY_NAME` + cópia com `SDL_IOFromFile`, ou pela Activity como o resto das importações. Ele roda
  em chinês: aceitar também as fontes zh-cn/zh-tw/ko (`nintendo_udsg-r_org_zh-cn_003` etc.; o .bfttf zh-cn usa outra
  chave/cabeçalho), senão o texto chinês some com a DejaVu.
- **#123 (Mali-G720, 0.2.4, 1.8.2 recompilado, cópias):** crash após 12 min: SIGSEGV write em 0x8, host pc
  f_100059380+420, depois de 31 linhas `global memory: shaders reach heap ... outside their tables` - a família que a
  correção do GMap do Argon (0.2.5) trata. Pedir a 0.2.5 e prints dos glitches.
- **#124 (Mali-G925, POCO X8 Pro Max, 1.8.2):** após 10-15 min glitch, 6 fps e fecha; no Game Hub da Xiaomi dura
  bem mais. Log de 18 linhas (não jogou). Família "crash depois de minutos em Mali" (hipótese dos VMAs). Pedir o
  log-previous logo depois do crash, na 0.2.5. Ofereceu doar.
- **#119 (Dimensity 8400 / Mali-G720, 0.2.4):** trocar de arma ou de poder cai para 2-9 fps; na 0.2.2 não caía.
  Suspeita: a 0.2.4 trocou as leituras de volta da Mali para memória cached com flush/invalidate manual, e o menu
  rápido usa a captura da tela (desfoque). Pedir log na 0.2.5 abrindo esse menu.
- **#125 (Galaxy A56, Xclipse 540, 0.2.3):** 2-5 fps, render thread 51 ms, GPU 1% - a regressão de memória não
  cacheada da 0.2.3 (corrigida na 0.2.4). Pedir a 0.2.5 (o #126 no Xclipse 530 chega a 20-30).
- **#127 (Mali-G57 MC2, driver de fábrica r32/Vulkan 1.1, 0.2.5, 1.9.0, 50%):** 6-12 fps, picos de 5 fps com 60 mil
  draws e 2,8 GB de memória de GPU. A GPU mais fraca testada até aqui (#85): fila de desempenho Mali.
- **#128 (LATAM Spanish):** o jogo tem o espanhol latino-americano (`Pack/Bootup_USes.pack`, código `es-419`) e o
  francês do Canadá (`USfr`, `fr-CA`); o app só oferece "es"/"fr" (Espanha/França). Fácil: duas opções novas em
  Idioma e, no Auto, es-MX/es-US/es-AR/es-419... -> es-419 e fr-CA -> fr-CA.
- **#120:** issue vazia ("BOTWRecomp_test log", sem texto nem anexo).
- **Melhoria do log:** nenhum log diz a versão do app - os de 30/10 tiveram de ser datados pela hora da release.
  Escrever `BOTWRecompAndroid 0.2.x (versionCode N)` na primeira linha.

**Triagem de 2026-10-31, madrugada:**
- **#129 (S24 Ultra, Adreno 750 no Turnip, 0.2.5) - regressão da 0.2.5:** faixas diagonais no terreno (grama e rocha
  alternadas) e o fundo do menu principal errado. Causa provável: o 7315a62 (PR #103) alargou `vk_turnip_path_on`
  inteiro para todo Turnip com `map_placed`, e essa função decide também as leituras in-place dos alvos
  (inplace_reads) - as decals do terreno leem e escrevem os alvos do G-buffer, um feedback loop só conferido no Adreno
  830. **Corrigido (local):** `vk_turnip_heap_on` só para o heap (o que o #103 precisava: sem ele o 7xx fica sem
  texto); inplace_reads e LRZ de volta a só 8xx; o rótulo "Automático (ligado/desligado nesta GPU)" e o texto do heap
  seguem a escolha real. Não reproduzível no POCO (830): pedir ao autor "Leitura direta da imagem desenhada" = Desligado para
  confirmar. Segunda suspeita: o arredondamento de x*255 do #112 (02ab939, `LARECOMP_NO_UNORM_ROUND=1`).
- **#115 (S24 Ultra + GameSir X2 Type-C, sem giroscópio):** o giroscópio do celular não funciona com o controle
  ligado, na 0.2.4 e na 0.2.5. Hipótese: o X2 anuncia um giroscópio pelo SDL que manda ruído, a 0.2.4 (>0,05 rad/s) e
  a 0.2.5 (20 leituras diferentes) o tomam por vivo e calam o do celular (pad_until_ns). Pedir o log (linhas
  `[motion]`). Corrigir com uma opção "Sensor de movimento: Auto / Celular / Controle".
- **#130 (sem aparelho nem versão):** um monte de folhas no chão que segue o Link (areia, água, rocha). Pedir GPU,
  versão do app e do jogo, modo e o log.
- **#131 (GJ3303, Mali-G720, 1.8.2, mesmo autor do #123):** crash depois de ~30 min ao sair de um santuário, repetido.
  SIGSEGV lendo 0x88 em f_100a1fb80+34 (outra função que o 0x8 do #123), 175 linhas `outside their tables`, rss
  4743 MB na corrida anterior. Família "crash depois de minutos em Mali". O log não diz a versão do app (aberta depois
  da release da 0.2.5): perguntar.
- **Respostas boas:** #108 (SD845) confirmou a 0.2.5 - voltar do segundo plano não crasha mais (fechada). #89
  (Mali-G925) jogou na 0.2.5 sem crash, só quedas de fps. #126 vai testar a 1.6.0 no Xclipse 530.

**Esperando log ou resposta:** #22 (vídeo da torre no Adreno 840), #24/#30 (8s Gen 4), #23 (RP Nova T26), #41
(build de teste com `uclamp.min`), #68 (S23 a 5-10 fps), #64 (build no Mali-G57).

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
- **Controles de toque estilo FPS** (pedido no Reddit, 2026-10-24) - feito e aprovado pelo usuário no POCO (2026-10-25): na aba
  Controles, "Câmera por arrasto" (`drag_frame`: inclinação total a 80% da altura da tela por segundo vezes a
  sensibilidade, suavizada, começando depois de 15% para passar a zona morta do jogo; usa a metade direita inteira),
  "Analógicos só ao tocar" e "Áreas maiores dos analógicos" (cada metade da tela). Ajustar no POCO: a curva e a zona
  morta do arrasto, e se a câmera fica com atraso. O pedido original:
  movimento e câmera pelas duas metades da tela,
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
- **Resolução dinâmica do jogo só no modo Dinâmica** (sugestão do usuário, 2026-10-25; há mods do BOTW que a
  desligam) - feito em 625f8c3: os contadores de GPU do jogo ficam congelados (a cena presa em 1600x900;
  `LARECOMP_GAME_DYNRES=1` devolve a do jogo), e Resolução interna > Dinâmica é a nossa, pelo tempo de GPU. O próprio jogo baixa a resolução da cena quando o quadro pesa (1600x900, 1440x810... no log `[post]
  scene`), e as folhas escuras do aspecto livre aparecem justamente nesses degraus; trocas de tamanho dos alvos também
  são candidatas a outros bugs. Ideia: com a Resolução interna num valor fixo, prender a escala do jogo no máximo, e
  deixar a dinâmica só quando a opção estiver em Dinâmica. Caminhos:
  - O jogo escolhe a escala pelos tempos de GPU que lê (`ReportCounter`): o pacer já os finge (com `frame_rate = 30`
    ele fica na escala mínima), então devolver tempos curtos pode prendê-la no máximo sem mexer no código do jogo.
  - Ou como os mods fazem (patch no exefs que trava a escala): conferir os endereços deles nas três versões.
  - Medir o custo: a escala dinâmica é o que segura os 30 fps nos lugares pesados; sem ela, menos fps lá.
- **Menus do app nos idiomas do jogo** (pedido do usuário, 2026-10-25) - feito, falta conferir no celular: uma
  tabela por idioma (`runtime/ui/i18n_<idioma>.inc`: es, fr, de, it, nl, ru, ja, ko, zh_hans, zh_hant, além do pt),
  escolhida pelo idioma do celular (`settings_menu_language`; `LARECOMP_HOST_LANG=ja` para testar); russo, japonês,
  coreano e chinês com os glifos que a tabela usa, vindos das fontes do próprio Android (NotoSansCJK, Roboto) e
  mesclados na Baloo 2 (`i18n_merge_fonts`); os textos do lado Java em `res/values-<idioma>`. Conferir no POCO:
  altura e alinhamento dos glifos mesclados (escala 0,6 da Baloo, a ajustar), largura dos textos mais longos
  (alemão, russo) nas linhas do menu, e que o NotoSansCJK existe no HyperOS.
- **Build do código do jogo mais leve** (issues #18 e #25, 2026-10-25) - feito na 0.1.11 (2f68d60, aabd205):
  compiladores novos só com memória livre, metade num celular quente, o build continua de onde parou, e o log diz
  por que o Android fechou o app. Escolher a pasta de novo ainda apagava os objetos (`.gamebuild`): corrigido em
  201e99b, ainda não lançado. No #25 (ZF8 Ultra) a 0.1.11 ainda esquentou demais: falta o log do build.
- **Próxima versão: o que as issues já esperam** (2026-10-26). No `main`, não lançados, e prometidos como "a próxima
  versão":
  - 201e99b: cada arquivo uma vez quando o provedor de arquivos lista em dobro (#31/#38: Huawei, 79.356 arquivos, uma
    imagem de 28,84 GB de uma pasta de 14,44 GB) e a pasta escolhida de novo mantém o código compilado (#25).
  - 526fc76 + 0464be0: relatório de crash um por vez, com os registradores e a cadeia de quadros do jogo (x29) antes
    do backtrace do host (#24, #30).
  - 4a27085: os marcos de cada vídeo (primeiro pacote, primeiro quadro decodificado, primeira saída ao jogo) (#22).
  - De 26/10, também prontos:
    - Saída Automática (60ac287);
    - o GLSL mais barato do SavageWindRecomp (9f94aba: menos travadas de shader na primeira vez);
    - as esperas com limite e o áudio do TOTK (0b86617);
    - a altura de bloco do NVN (e82107e);
    - o MapMemory que confere endereços e a mensagem de build com a causa (4a27085).
    - As notas precisam explicar que os shaders compilam de novo uma vez e que a Saída vira Automática.
  - Antes de lançar, testar no POCO: a verificação do setup num caso que falha (uma cópia em
    `/sdcard/Download/botwtest` com o `Version.txt` trocado; nunca nas pastas do usuário) e o relatório de crash (não
    há como provocar um crash de propósito: `LARECOMP_TEST_CRASH=<s>`, e346bc7, faz duas threads do jogo caírem).
  - Depois de lançar, avisar em #22, #24, #25, #30 e #38.
- **`uclamp.min` para o #41** (prometido ao autor, 2026-10-26): Snapdragon 8 Gen 3 (Adreno 750, Honor 300 Ultra,
  MagicOS) cai para ~10 fps no mundo com um núcleo ocupado a ~600 MHz. O trabalho por quadro é o mesmo do POCO, ou
  menos (3.000-4.500 draws e ~15 MB enviados por quadro, contra 4.500-6.000 e 20-25 MB a 30 fps), e a sessão do ADPF
  está ligada: algo acima do escalonador segura o clock (o iAware do MagicOS gerencia o app).
  - Feito em edcbe12 (falta rodar no celular): `LARECOMP_UCLAMP_MIN=<1-1024>` aplica `SCHED_FLAG_UTIL_CLAMP_MIN` às
    threads de `plat_thread_unpin` e diz no log se o Android aceitou; e o núcleo 0 ganhou uma linha a cada 30 s (trocas,
    esperas, quanto do tempo ficou ocupado). Mandar ao autor um build de teste.
  - A lista de jogos e o modo desempenho do MagicOS não mudaram os clocks (resposta dele, 2026-10-27).
- **DLC** (#42, #40, #34; talvez o crash ao chamar o cavalo do #43) - em andamento numa sessão própria (pedido do
  usuário, 2026-10-25). O Modo Mestre vem com o primeiro pacote (#34). Avisar nas issues quando sair.
  - Os dois pacotes extraídos para testar (2026-10-27, `romfs/` de cada um):
    `D:\Projetos\Jogos\Android\Updates_Switch\work_dlc_master_trials` e `...\work_dlc_champions_ballad`.
  - O trabalho em andamento (não commitado): `runtime/services/fs.c`, `runtime/services/misc.c`, `runtime/hle/movie.c`.
  - Feito em 15c8323 (2026-10-27): Configurações > DLC instala a imagem (`dlc/aoc.bin`, 52 s no POCO), e a 1.8.2
    mostra Modo mestre, "Conteúdo extra adquirido" e "DLC Ver.3.0".
  - Visto uma vez: na tela de título com o Modo mestre escolhido, o menu ficou em câmera lenta (com 30 fps estáveis no
    log, sem compilações nem esperas) e depois voltou ao normal sozinho; talvez enquanto o jogo carrega os recursos do
    modo. Observar.
  - Falta testar jogando: o Modo mestre, a Provação da Espada, a Balada e os itens do DLC (e o cavalo do #43).
- **Importar saves de emulador** (prometido no #40, 2026-10-26): os saves ficam em
  `Android/data/com.botwrecomp.android/files/<versão>/save/save_0`, fora do alcance dos gerenciadores de arquivos (nem
  o adb escreve ali: só o próprio app). Os emuladores de Switch guardam os do BOTW no mesmo formato (`0/` a `5/` com
  `caption.jpg`, `caption.sav`, `game_data.sav`, `option.sav`, `tracker/`).
  - Feito em 3d66b47 (falta testar no celular): Configurações > Saves, com Exportar (uma pasta datada onde o jogador
    escolher; também o #49, que queria editar os saves) e Importar (acha o slot com `game_data.sav` até quatro níveis
    abaixo; o save anterior fica como `save_0.before-import`).
  - Conferir no celular: uma pasta de emulador real, e um save mais novo que a versão (um 1.8.2 numa 1.0.0).
- **Crash no Snapdragon 8s Gen 4 / Adreno 825** (#24, #30; POCO F7): sempre no mesmo ponto, ~2 s depois de a tela de
  título começar a carregar, em todo driver (Turnip, Qualcomm), com a memória do jogo na GPU ligada ou não e em
  toda resolução. É no código do jogo (1.0.0, `f_100a550e0+0x94`: um append de lista com o elemento nulo, centenas
  de chamadores), com duas threads do jogo no mesmo nulo, 33 ms uma depois da outra. O código do jogo sai igual em todo
  celular (`-target aarch64-linux-musl` sem `-mcpu`). No POCO a 1.0.0 passa desse ponto. Falta o log da próxima
  versão (526fc76: os chamadores pela cadeia x29) para achar o objeto nulo.
- **Vídeo da torre trava no Adreno 840** (#22; OnePlus 15, Snapdragon 8 Elite Gen 5, 1.0.0 com o dump completo): o
  jogo inteiro para logo depois de `[movie] decoder vp9 configured`, com Turnip e com o driver da Qualcomm; no
  Adreno 830 o mesmo vídeo passa até o fim. O VP9 do FFmpeg 9.0.2 só tem caminhos NEON (os de SVE/SME são do VVC).
  O log com os marcos de 4a27085 (2026-10-28): `Demo166_0.webm` aberto, `video decoder: first packet: 242 bytes` e
  nenhum "first frame decoded" depois - o decodificador recebe o primeiro pacote e nunca devolve um quadro. Na 1.8.2
  o mesmo usuário passa da torre.
- **#43** (Adreno 740, 1.8.2, Turnip V38): a grama some de vez em quando ao andar, e chamar o cavalo trava com um
  save da 1.6.0. Esperando o log (e se o cavalo usa equipamento de DLC).
  - **A grama:** reproduzida no POCO (1.9.0, Grande Platô, os degraus ao lado da torre, 2026-10-27): a grama baixa
    perto do Link some entre um quadro e outro (as touceiras altas ficam). Igual com `LARECOMP_SYNC_LAG=0` - não é o
    sync lag, e já estava na 0.1.11. Suspeitas: a máscara de densidade da grama, que a GPU desenha e a CPU lê para
    escolher as células (o MacNativeGames trata esse caminho), ou o fade de perto da grama baixa. Próximo passo: um
    quadro de depuração (F9) com a grama presente e outro sem, e comparar os draws da grama. O PR #511 do MacNativeGames
    também corrigiu "grama alta cobrindo a estrada perto do Link": uma cópia amostrada de um tile de material do
    terreno (128x128) que ficava velha - conferir se o nosso caso é esse.
  - **A malha de linhas escuras** (#21, desde a 0.1.x): reproduzida no POCO (1.9.0, mesmo lugar, 2026-10-27). Linhas
    pontilhadas horizontais e verticais presas ao terreno (uma grade no chão), visíveis só ao mexer a câmera. Continua
    com o sync lag desligado, com as leituras no lugar desligadas e com o LRZ desligado. Suspeita: frestas entre os
    tiles do terreno (os vértices da borda de dois tiles vizinhos com alturas um pouco diferentes). Próximo passo:
    comparar as alturas das bordas de dois tiles no shader do terreno (o mesmo caminho do atlas de alturas e das cortinas).
    **Corrigido** (2026-10-27): com filtragem nearest nos vertex shaders as frestas sumiam; a causa é o peso bilinear
    (o Switch usa 8 bits de posição sub-texel, o Adreno mais). As coordenadas dos vertex shaders agora são arredondadas a
    1/256 de texel (`LARECOMP_NO_VS_SNAP=1` desliga). Testado no POCO: sem frestas, terreno normal. Avisar no #21.
  - **Os pontinhos que ficaram** (2026-10-28, POCO, 1.9.0, recompilado e nativo, cena a 1600x900): fileiras de pontos
    escuros em perspectiva, alinhadas ao terreno, **só no chão gramado** (não na rocha, nem do lado da pedra sem grama),
    mais visíveis em resoluções acima de 50%. Não é o mesmo defeito das frestas:
    - Descartados, um por vez: `LARECOMP_VS_NEAREST=1`, `LARECOMP_NO_VS_SNAP=1`, o arredondamento também nos fragment
      shaders (`LARECOMP_FS_SNAP=1`, diagnóstico novo), buracos na profundidade (o fbdepth do quadro de depuração quase
      não tem), a profundidade em meia resolução (800x450 R32F, limpa), o depth bias (`LARECOMP_D16_BIAS_SCALE=256`,
      diagnóstico novo; o jogo nem usa polygon offset nas sombras: factor 0, units 0), a comparação com o mapa de
      sombra (a iluminação diferida `aaed7dc1f198ccc2` com o sol sempre "aceso", por `LARECOMP_SHADER_SUBST`), a
      máscara de densidade da grama lida pela CPU (`LARECOMP_WRITEBACK_MAX=1048576`: a 512x512 volta, os pontos
      ficam), os cartões de grama (`LARECOMP_SKIP_FSHASH=477912f75d2e8819`) e o índice de material do terreno gramado
      lido por `texelFetch` (substituição do fs `60f90627d72c7d08`).
    - Onde estão: o terreno gramado é desenhado por `50fc37edcd79cf02` / `60f90627d72c7d08` (pulando os dois e o
      `477912...`, o chão gramado some e a rocha fica); os pontos já estão na iluminação em meia resolução (pass 167,
      800x450) e no primeiro passe de cor depois dela. O terreno não grava normal no alvo de normais do G-buffer
      (1600x900 f0x25 @3cda43600: preto no chão).
    - Também descartados: profundidade do terreno empurrada para trás, Turnip sem bins (`TU_DEBUG=sysmem`), cópias em
      vez de leituras no lugar (`LARECOMP_FEEDBACK_COPY=1`), o composto `f607b5ab4c2f0720` sem o deslocamento por
      profundidade e o ruído 4x4 do AO fixo. Os pontos são **alinhados à tela** (colunas a cada 50 px de 1600x900), não ao
      terreno.
    - **Corrigido** (2026-10-28): a história do pixel (`build/pick.txt`, agora com `rt=<i>`) mostrou o valor escuro
      nascendo no azul (oclusão de ambiente) do `8c42c9bc921ed716`, a luz em meia resolução (800x450, alvo 5). A
      profundidade central do AO vem de um `textureGather` na própria coordenada do pixel: no centro de um texel o 2x2
      depende do último bit; o Switch arredonda a 1/256 de texel e fica com o texel do pixel, o Adreno às vezes pegava o
      2x2 anterior (a profundidade do vizinho: escuro junto às folhas). Os gathers agora arredondam a coordenada a 1/256
      de texel e somam 1/1024 (`LARECOMP_NO_GATHER_SNAP=1` desliga). Testado no POCO: sem pontos, grama e sombras normais.
      Ferramentas: `LARECOMP_DUMP_FS=<hash>+<hash>`, `LARECOMP_SHADER_SUBST=<pasta>` (`<code>_fs.glsl`),
      `LARECOMP_DEBUG_PICK` / `build/pick.txt` (`x,y;x,y rt=<i>`).
  - **A grama que só aparece de perto** (relato de 2026-10-28): a camada de 50 a 130 m depende da máscara de densidade
    (512x512 RGBA8 e 256x256 RG8) que a CPU lê de volta; o runtime só devolve alvos até 4 KB (ou lineares), então ela
    nunca volta. O MacNativeGames corrigiu o mesmo (f1043e5, e o PR #829 tira a espera). A fazer: devolver esses alvos,
    de preferência ao fim da submissão deles, sem esperar a GPU.
- **#23** (RP Nova T26, 1.0.0): o `ActorInfo.product.sbyml` está na pasta, mas a imagem tinha 39.677 arquivos e o
  jogo para ao carregá-lo. Esperando a mensagem da verificação de setup da 0.1.11 (qual arquivo falta) e o tamanho
  exato do arquivo (760.223 bytes).
- **Mali** (#39: Dimensity 7050, Mali-G68, 7-10 fps; #34: Mali-G925, crash no código do jogo em `f_100047d40`;
  #29): o caminho da Mali nunca foi ajustado, e não há um celular Mali para testar.
- **Build no Galaxy S21 (Snapdragon 888)** (Reddit, Salt-Alfalfa-8665, 2026-10-26).
  - Na 0.1.11, o build **recomeçou do zero** apesar de continuar de onde parou. Suspeitas:
    - a pasta escolhida de novo apagava os objetos (corrigido em 201e99b, não lançado);
    - ou a bateria acabou e o app foi morto antes de o `obj.ok` ser escrito.
  - Ele também escreveu "doesn't fully scan now": a verificação do setup não termina? Falta saber em que tela.
  - O app **caiu em 81%**. Falta o log, com o motivo do Android (`the last run ended`).
  - Pede o build **em segundo plano**: feito em 97afeda (falta testar no celular), um foreground service com a
    porcentagem numa notificação e um wake lock parcial enquanto o build roda. Só carregando: não feito (o texto da
    notificação pede para deixar carregando).
  - Perguntar o modelo de RAM, e pedir o `botwrecomp-log.txt` e o `botwrecomp-log-previous.txt`.
  - O mesmo em 75-85% no #25 (OnePlus 15, ZenFone 8 Ultra, 1.6.0) e no #47 (Snapdragon 888, 1.6.0). O log do #47
    diz **"the last run ended: low memory"**: o Android matou o app no build, com 1-1,9 GB livres e status térmico 3.
  - Causa provável achada (b8dec17, falta testar no celular): na 1.6.0 o `main_536.c` tem uma função de 167.749 linhas
    com 14.525 rótulos, abaixo do limite de 15.000, então em -O1: 1,1 GB por 526 s no PC (`tools/cc_memory.py`; o
    resto da 1.6.0 fica abaixo de 350 MB). No celular, dezenas de minutos com mais de 1 GB enquanto os outros
    compiladores levavam a barra de ~53% a ~80%. Agora uma função com mais de 60.000 linhas vai para -O0 e o arquivo
    compila sozinho. A 1.8.2 e a 1.9.0 não têm função assim: se o #51 for da 1.8.2, a causa é outra.
  - E o gerador (Python, dentro do app, ao lado dos compiladores) guardava o C de toda instrução até o fim: 3,3 GB
    na 1.6.0 e 2,1 GB na 1.8.2. Desde 9603800 guarda só os desvios: 1,2 GB e 0,8 GB, a saída idêntica, ~25% mais
    tempo de gerador. Isso vale para todas as versões, inclusive a 1.8.2 do #51.
  - E o zig cc: com -O0 ele entra no modo Debug e põe UBSan, stack protector e debug info sozinho (e debug info no
    -O1, que ele compila como -O2). O `main_536` levou 6-8 GB por 20+ minutos no POCO até o Android matar o app
    ("swap is low and thrashing"). Com `-fno-sanitize=undefined -fno-stack-protector -g0` (6582d40), menos de 20 s.
  - Testado no POCO (2026-10-27): o build da 1.6.0 terminou (876 s, retomando os 675 arquivos de antes), com o serviço
    em primeiro plano e a tela apagada, e o jogo roda com esse código.
- **Cortinas de terreno depois da cutscene de uma torre** (2026-10-26, em andamento).
  - Aparecem só quando a ativação toca a cutscene. Carregar um save já com a torre ativada não mostra nenhuma.
  - Com `LARECOMP_TEX_FULLHASH=1` não apareceram. Não é o padrão do #511 do MacNativeGames
    (`LARECOMP_LOG_STALE_UPLOAD` só pegou o vídeo).
  - Suspeitas: a comparação por snapshot das texturas dinâmicas (`texture_unchanged`), ou uma corrida com as threads do
    jogo que escrevem texturas enquanto a thread de render as lê.
  - Medindo o custo da verificação completa antes de decidir a correção.
- **Sync lag**: ligado por padrão desde e5d6f00 (46,6 -> 53,6 fps no A/B com o Turnip V38, a água certa). A água
  branca vinha dos write-backs descartados com os slots cheios; desde 502088f eles esperam a GPU.
- **BOTW 1.9.0** (#28, em andamento, 2026-10-27). SDK 21.4.1, main `cd57b23f`, romfs com 41.062 arquivos.
  - Reconhecida (pasta `v190`, launcher, romfs, am), com o código de 1.8.2 para SDK 20 / NVN 55.
  - Os endereços de 1.8.2 (pacing, VFR, widescreen, laço de quadros, pausa, telas, eventos, passo do controle) achados
    por `tools/port_addrs.py`, cada um com uma só correspondência e as mesmas instruções.
  - Roda (f0df56c): o título com "Ver. 1.9.0", um jogo novo, e um save da 1.8.2 no Grande Platô a 59,5 fps.
  - Falta: widescreen, uma sessão longa, e as chamadas novas do SDK (o MacNativeGames cita `PopOpenUsers` e
    `pl::IsRunningOnOunce`; no log, `ICommonStateGetter` 54/66, `IApplicationFunctions` 67 e `ISelfController` 16
    sem resposta, e o jogo seguiu).
- **Os 16 bytes por draw (feito, 2026-10-27):** o jogo reescreve 16 bytes de um constant buffer de 64 KB antes de cada
  draw, e cada draw copiava os 64 KB (~14 MB por quadro). Na 1.9.0 as escritas caem em linhas diferentes dos primeiros
  656 bytes (não só +0x70), então em vez de push constants: o buffer é lido no lugar no heap, e o draw copia só as
  linhas que as escritas alcançam (até 64) para um buffer de remendo por estágio (binding 23, `CBP`), de onde os
  shaders com esse buffer indexado por registrador as leem (`maxwell_cb_window`, `cbwin_bind`). A/B na mesma rodada
  (`LARECOMP_AB=60:cbwin`, 1.9.0 no Grande Platô, celular a ~88 °C): render 13,81 -> 13,49 ms, GPU 14,82 -> 14,57 ms,
  período 19,28 -> 18,94 ms; imagem igual. Ganho pequeno: o memcpy era mais barato do que a estimativa. O período
  (~19 ms) passa da soma esperada: a thread de render (~13,5 ms) e a GPU (~14,6 ms) ainda andam em parte em série.
  **Desligado por padrão desde o mesmo dia** (`LARECOMP_CB_WINDOW=1` liga): em jogo a água ficou branca, o terreno
  em "cortinas" e objetos ao longe com pontas num golpe - o buffer lido no lugar enquanto a CPU já escreve o quadro
  seguinte nele (o sync lag deixa a CPU um quadro à frente). `LARECOMP_LOG_CBWIN=1` mostra o que fica copiado.
- **Onde vai o quadro de 60 fps (1.9.0, Grande Platô, 2026-10-27, celular a 75-93 °C):**
  - Linha do tempo (`LARECOMP_FRAME_TIMELINE=4000`), ~19 ms por quadro, em série: o jogo grava a lista do quadro
    (~3,9 ms, uma lista só), a thread de render traduz (~11 ms, 6-8 submissões pelo split de 3 ms) e apresenta (~0,8
    ms). O jogo só volta ~3 ms depois do fim do present: antes disso a thread que apresenta fica ~15 ms bloqueada
    esperando as outras threads do jogo (a lógica), não a nossa. Nenhuma thread satura (`top -H`: a mais ocupada 74%),
    e o núcleo prime fica em ~2,2 de 4,1 GHz.
  - Testado sem ganho (A/B na mesma rodada): a sessão ADPF com as 8 threads do jogo mais ocupadas e o período real do
    quadro como trabalho (17,94 vs 17,83 ms; com as ~40 threads o Android recusa a sessão); o período sozinho (18,05
    vs 18,06). O HyperOS não sobe os clocks por essas dicas. Sem o revezamento do núcleo 0 (`LARECOMP_NO_CORE0=1`,
    rodadas separadas): 18,21 vs 18,27 ms - também não é o limite.
  - GPU ~14,2 ms de 16,7, render ~11,8 ms. O que sobra: menos trabalho de GPU e de lógica a clocks baixos; o calor
    (90+ °C em poucos minutos, carregando) corta os clocks pela metade.
  - Passes da GPU (`LARECOMP_GPUPROF_AT=3000`, com `LARECOMP_GPUPROF_DRAWS=1`): os três passes do G-buffer (6 alvos,
    1600x900, ~2.400 draws) ~6,6 ms; a saída com FSR 1 (EASU 0,70 + RCAS 0,26 ms) e FXAA para 2560x1440 (0,80 ms);
    nenhum shader domina (o maior 10%, ~4 us por draw no resto): a GPU paga por draw. A primeira linha do quadro (~3
    ms) é ociosidade esperando a próxima submissão.
  - **A GPU não é o limite agora:** SGSR no lugar do FSR 1 (`LARECOMP_AB=120:sgsr`) tirou 0,5 ms de GPU (14,15 ->
    13,65) e o período ficou igual (17,91 vs 18,03 ms). O quadro é a cadeia de CPU do jogo (a lógica nas threads
    dele, depois a gravação da lista), a esses clocks.
  - As threads do jogo que mais rodaram (a cada ~2 s, por `/proc/self/task/<tid>/schedstat`) presas nos 2 núcleos
    prime (A/B na mesma rodada): 2 threads 17,62 vs 17,62 ms, 4 threads 18,24 vs 18,29 ms. Sem efeito, desfeito.
  - **O que a thread que apresenta espera** (`LARECOMP_PROF_SVC=1`, `LARECOMP_PROF_SVC_TID=80+82`,
    `LARECOMP_PROF_SVC_FROM=75`; `[pace] presenting thread` diz qual é): a thread de desenho do jogo (t82, handle
    0x118) espera ~16 ms por quadro numa variável de condição (chave 3446c5148), sinalizada uma vez por quadro pela
    thread principal (t80, handle 0x101, chamada do laço de quadros da 1.9.0 em 0x11267c4). Nesse tempo a t80 quase
    não espera: só sincronizações curtas com os jobs (`WaitForAddress`, 0,3-1 ms) e yields (`SleepThread(-1)`, 1-2
    ms, que entregam o núcleo 0). **O caminho crítico é a CPU da thread principal do jogo, ~12-15 ms por quadro de
    código recompilado numa thread só.** Para 60 fps estáveis: deixar esse código mais rápido (o que ele roda: falta
    um perfil por amostragem da t80, o `fnprof` só conta entradas) ou clocks mais altos nessa thread.
  - **Referência: [totk-mobile](https://github.com/OJZen/totk-mobile)** (OJZen, Apache-2.0; o núcleo vem do
    d2r-mobile, MIT) e o [MacNativeGames](https://github.com/ly05010419/MacNativeGames) (MIT) não recompilam: rodam o
    código ARM64 do jogo direto na CPU, com um reescritor estático do módulo (`core/tools/patch_nso.py`: x18 e
    `tpidr_el0` em slots da thread, `mrs tpidrro_el0` no TLR do Horizon, `svc` e `mrs cntpct_el0` em portas para o
    host). Para o nosso caminho crítico (a thread principal do jogo) seria o maior ganho possível, e acabaria com a
    compilação do código do jogo no celular - mas é outra arquitetura. Existe um port para Android do MacNativeGames
    (deivid22srk/MacNativeGames-Android) a estudar antes. Nada foi copiado deles até agora.
- **Pontas na nuvem da Montanha da Morte e na névoa do horizonte num golpe** (2026-10-27, em aberto; cosmético,
  um quadro). Visto pelo usuário na 0.2.0, 0.2.1 e no branch native-p0, recompilado e nativo, a 30 e 60 fps.
  - Descartados (cada um num teste em jogo): Sync lag, `LARECOMP_SYNC_ALL=1` (a GPU esperada por inteiro: não é
    corrida CPU/GPU), `LARECOMP_FRAMEPACE=0` (o relógio do próprio jogo), `LARECOMP_NO_VS_SNAP=1`, o caminho dos 64 KB,
    o modo FMZ do FMUL/FFMA (implementado e testado, sem efeito, desfeito).
  - O draw: passe de meia resolução 800x450 (o 209 de 269 no quadro de depuração 9490), 354 vértices, 8 atributos,
    fs `8d989710b28bd28a`, vs em 0x36e610000 (`LARECOMP_DUMP_FS`): partículas - idade = `cb10[2].x` (tempo do
    efeito) - nascimento (`a5.w`), vida `a4.w`, arrasto `cb9[12].x`, eixos de câmera de `cb10[8..10]`, duas texturas
    lidas no VS.
  - `LARECOMP_LOG_FS_CB=<fs>:<estágio>:<cbuf>:<linha>:<linhas>` (novo) registra o cbuf por quadro: o `cb10` está são em
    todos os golpes - o hit-stop é o tempo do efeito a 10% (passo 0,05) voltando em rampa 0,1, 0,2, 0,4; no quadro em
    que começa, o tempo ainda andou 0,5 e `cb10[2].w` já diz 0,05.
  - Também descartados: leitura além do fluxo de vértices (o draw usa só o fluxo 0, 120 vértices de 20 bytes, e os
    índices nunca passaram do tamanho ligado, medido em vários golpes) e textura atualizada sem ser notada
    (`LARECOMP_TEX_FULLHASH=1`: as pontas continuam).
  - Parado aqui (cosmético, um quadro). Próximo, se retomado: o vs calculado na CPU com os vértices e cbufs do quadro
    de transição do hit-stop, ou a tradução do Ryujinx do mesmo shader para comparar.
  - **Corrigido** (2026-10-28). O `8d989710` era o draw errado: pulado (`LARECOMP_SKIP_FSHASH`), a fumaça e a névoa
    continuavam. A história do pixel na nuvem achou o `775680b66449d03a` (fluxo 800x450 f0x29; pulado, a nuvem larga
    some), que estica cada partícula pela velocidade (`a6.xyz`, girada pela matriz do emissor) vezes `1 / cb10[2].w`.
    `cb10[2].w` é o delta frame do jogo (1,0 a 30 fps, 0,5 a 60) vezes o multiplicador de tempo mais lento: no hit-stop
    0,05, então o esticamento cresce 10x por alguns quadros (a fumaça dos monstros derrotados também). A 30 fps estica
    igual: é do próprio jogo. A pedido do usuário, o tradutor lê esse passo, só no recíproco de `c[10][0x2c]` dos vertex
    shaders, como no mínimo 0,5 (`LARECOMP_NO_STEP_CLAMP=1` desliga). Só shaders de partículas leem `cb10[2].w`; os
    outros o usam como `idade + w`, que fica igual. Testado no POCO a 60 fps: sem pontas.
    Ferramentas novas: `LARECOMP_DEBUG_ON_FS_CB=<fs>:<estágio>:<cbuf>:<float>:<abaixo>` (quadro de depuração quando o
    valor cai: o início do hit-stop), `LARECOMP_LOG_FS_VB=<fs>:<fluxo>`, `LARECOMP_SHADER_PATCH=<arquivo>` (troca de texto
    no GLSL dos programas com aquele fs), e o `LARECOMP_DEBUG_DATA` mostra os fluxos por instância (2 a 7).
- **Aquecimento de shaders com listas conhecidas** (como o MacNativeGames): menos travadas no primeiro jogo. Médio.
  Pedido de novo pelo usuário em 2026-10-30 ("preparar os shaders antes de iniciar o jogo"). Hoje entre sessões já
  ficam o SPIR-V de cada shader (`cache/spirv`, chave pelo GLSL) e o pipeline cache do driver
  (`cache/vk_pipelines.bin`); na primeira vez o draw é pulado enquanto compila (sem travar, mas some por alguns
  quadros), e nas seguintes ainda custa a tradução NVN -> GLSL e a criação do pipeline. Falta:
  - **Lista dos programas vistos:** gravar por sessão a chave de cada pipeline criado (endereço/hash do vs e do fs mais
    o estado que entra no pipeline: formatos dos alvos, layout dos vértices, blend, topologia) num arquivo por versão
    do jogo.
  - **Pré-aquecimento no carregamento:** no boot (ou na tela de título), recriar esses pipelines em threads de fundo
    antes de o jogador entrar no mundo, com uma barra/percentual; com o pipeline cache do driver, é rápido.
  - **Para quem nunca jogou:** traduzir todos os shaders do romfs (bfsha) na configuração dá o SPIR-V, mas não o
    pipeline completo (o estado só se conhece no draw): cobre só parte. Uma lista de chaves comum, gerada aqui e
    distribuída com o app, não leva conteúdo do jogo (só hashes e estado) - avaliar.
  - Medir antes/depois: draws pulados e `compile N (waited)` no log `[vk] t=` ao entrar em áreas novas.
- **Pequenos, do SavageWindRecomp e do MacNativeGames:**
  - feitos em b512a4b (falta testar no celular): o desvio do giroscópio em repouso aprendido e descontado, e a zona
    morta dos analógicos do controle (Configurações > Controles, 0-40%);
  - campo de visão: precisa de um patch no código do jogo em cada versão (achar o valor da câmera);
  - um botão 60/30 nos controles de toque: um controle novo no layout editável.
- **Espanhol da América Latina e francês do Canadá** (#128, 2026-10-30): o jogo já tem os dois (`Bootup_USes.pack` /
  `es-419`, `Bootup_USfr.pack` / `fr-CA`); faltam as opções em Idioma (settings.c g_language_codes/names, as traduções
  dos nomes) e o Auto mapear o país do celular. Pequeno.
- **Sensor de movimento: Automático / Celular / Controle** (#115, prometido na resposta de 2026-10-31): com um GameSir
  X2 Type-C (sem giroscópio) o do celular parou de funcionar na 0.2.4/0.2.5 - provavelmente o controle anuncia um
  giroscópio com ruído e a troca automática (motion.c motion_event) o toma por vivo. Uma opção em Controles, e o
  Automático conferir pelas linhas `[motion]` do log que o autor mandar.

## Regras

- Nada da Nintendo no repositório: nem o dump, nem código gerado, nem assets (ver `CONTRIBUTING.md`).
- Mudanças no motor que não dependem do BOTW devem ficar fáceis de levar de volta ao LAS (e vice-versa).
