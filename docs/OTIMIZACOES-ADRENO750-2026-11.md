# Otimizações medidas num Snapdragon 8 Gen 3 (Adreno 750, Turnip): novembro de 2026

Relato de um usuário (POCO F7 Pro, Xiaomi 24117RK2CG, SM8650, 12 GB, HyperOS 3 / Android 16), com testes feitos num
build próprio entre a 0.4.3-beta e a 0.4.6-beta. Tudo aqui foi medido no aparelho.

O documento tem três partes:
- as mudanças que viraram PR, com o estado de cada uma;
- configurações e tentativas que usam só o que já existe no app;
- no fim, **"Testado fora do repositório"**: código que existiu apenas no build de teste. As variáveis dessa seção não
  existem no app.

- **Driver:** Turnip "Gen8 V37" (StevenMXZ, Mesa 26.3-devel). Com o driver da Qualcomm o jogo ficava em ~19 fps
  (`no heap import`); outros Turnip testados (Mr Purple T26, KIMCHI v26 R2) ficaram em ~9 fps.
- **Jogo:** 1.8.2, modo Nativo, arquivos no lugar. Só a 1.8.2 foi testada.
- **Configuração de uso:** 30 fps, saída 720p, resolução "Igual à tela", SGSR 1, sem anti-aliasing.

| Mudança | PR | Estado (24/11/2026) |
|---|---|---|
| Relógio: o último uso de um pipeline pelo `g_exec_s` | #452 | **mesclado** |
| Fences anotados durante a cópia da lista | #456 | **mesclado** |
| Janela de constant buffers com uma cópia por submissão | #453 | **mesclado** (18/11) |
| Slots de textura na fila em todos os aparelhos | #454 | **mesclado** (20/11) |
| Modo 40 / 45 pela maior taxa da tela (JNI) | #455 | **mesclado** (18/11) |
| Opção Vídeo > Sombras | #457 | **mesclado** (18/11) |

## Como foi medido

- Celular **fora da tomada** (carregando, ele esquenta sozinho e distorce as medições) e adb por Wi-Fi. Os últimos testes
  (17/11, à noite) foram feitos com o celular num cooler.
- `LARECOMP_AB=<quadros>:<chave>` para comparar os dois lados na mesma partida e no mesmo calor;
  `LARECOMP_LOG_PACE=1` (render / GPU / período por quadro) e `LARECOMP_LOG_POWER=1` (W e °C por segundo).
- Mudanças que só valem na inicialização: rodadas alternadas A-B-A.
- Perfil: `simpleperf` (o aparelho tem root) com a `libmain.so` do build (-g) para os símbolos.
- As variáveis `LARECOMP_*` foram passadas pelo campo `driver_env` do `.ini` (faz `setenv` de qualquer `NOME=valor`)
  ou por `am start ... --es env A=1,B=2` - útil para testar num APK de release.

## 1. Relógio fora do `draw_` (`exec.c`) - #452, mesclado

**Problema.** `draw_()` gravava `pp->used = SDL_GetTicksNS()` em todo draw (~4.000 por quadro) para o `pipe_evict`.
Neste kernel o `clock_gettime` do vDSO é caro: **8,8% da thread de render** no `simpleperf`, ~95% vindo dessa linha.

**Como entrou.** O último uso de um pipeline vem do `g_exec_s` (segundos, atualizado a cada submissão), o mesmo relógio
dos programas: `Pipe.used_s`, e o `pipe_evict` conta em segundos como o `prog_evict`. Nenhuma leitura de relógio por
draw.

**Resultado** (da primeira versão, com uma leitura por passe; A/B na mesma partida, ~2.570 quadros por lado):

| | render | GPU | quadros > 40 ms |
|---|---|---|---|
| sem leitura por draw | **23,9 ms** | 27,0 ms | **193** |
| leitura por draw (antes) | 25,2 ms | 26,6 ms | 223 |

`__kernel_clock_gettime` caiu de 8,8% para 0,8% da thread de render: **−5% na thread de render, −13% de quadros
lentos.** A versão mesclada lê o relógio ainda menos.

## 2. Uma passada a menos por lista enviada (`async.c`) - #456, mesclado

**Problema.** `vk_async_execute` copiava a lista (`snap_add`) e depois `mark_fences` percorria a cópia inteira de novo
só para achar `CMD_FENCE_SYNC`: **~19% da thread que apresenta** no `simpleperf`. O `getenv("LARECOMP_NO_LIST_COPY")`
também rodava a cada lista.

**Como entrou.** `snap_add` anota a posição dos fences enquanto copia (até 64) e `mark_fence()` marca só eles. Acima de
64 fences, ou quando uma chamada mais funda que 8 é copiada como registro, volta a passada completa. `getenv` lido uma
vez. Ganho não medido isoladamente (estimativa ~0,4 ms por quadro na thread do jogo).

## 3. Janela de constant buffers com uma cópia por submissão (`exec.c`, `cbwin_bind`) - #453, mesclado

**Problema.** O buffer de 64 KB que o jogo reescreve (16 bytes antes de quase todo draw) é copiado inteiro por draw:
**~1.200 MB a cada 2 s** (`LARECOMP_LOG_CBUF_BYTES`). O `LARECOMP_CB_WINDOW` evita isso, mas liga o buffer **no lugar**
do heap, e com o Sync lag a CPU já escreve o quadro seguinte ali: água branca, "cortinas" (o motivo de estar desligado
desde 27/10).

**Como entrou.** No `cbwin_bind`, o resto do buffer vem de **uma cópia por submissão** (cache por endereço, tamanho e
`vk_pending_seq()`), que as atualizações não invalidam - as linhas atualizadas já vêm do patch buffer da janela.
`LARECOMP_CBWIN_SNAPSHOT=0` volta à leitura no lugar. A janela continua desligada por padrão no Adreno
(`LARECOMP_CB_WINDOW=1` liga); depois, o #506 levou o cache aos Mali em modo de cópia, onde ela passou a vir ligada.

**Resultado** (`LARECOMP_CB_WINDOW=1`, A/B `cbwin`):

| | cópias de constant buffer | render | potência |
|---|---|---|---|
| janela + cópia por submissão | **~92 MB / 2 s** | 19,67 ms | 5,58 W |
| cópia por draw (padrão atual) | ~1.200 MB / 2 s | 19,95 ms | 5,64 W |

**13× menos bytes copiados** (~15 → ~1,1 MB por quadro). O tempo cai pouco aqui (−0,3 ms: o memcpy deste SoC é rápido),
mas é tráfego de memória a menos. Água e terreno conferidos em jogo na 1.8.2, sem artefatos; o usuário joga com a janela
ligada desde então.

## 4. Slots de textura na fila, sem esperar a thread de render (`objects.c`) - #454, mesclado

**Problema.** Fora do low-RAM e do Mali, `register_texture` com o slot já ocupado chamava `vk_async_drain()`: a thread
do jogo esperava a thread de render **ficar ociosa** (terminar a lista e apresentar). ~940 vezes por segundo. Na linha
do tempo (`LARECOMP_FRAME_TIMELINE`) o jogo só mandava a lista seguinte ~1,5 ms depois do `present end`: jogo e render
nunca se sobrepunham, e a GPU ficava ~3,4 ms parada por quadro (`LARECOMP_LOG_GPU_IDLE`).

**Como entrou.** O caminho da fila (`vk_async_call(slot_apply)`), que era o padrão no low-RAM desde a 0.4.1 e no Mali
desde o #485, vale para todos os aparelhos (`vk_slot_async_default()` em `device.c`). `LARECOMP_SLOT_DRAIN=1` volta à
espera.

**Resultado a 60 fps na versão que entrou** (0.4.7 + o PR, parado numa casa de Kakariko, 2 min por rodada, alternando):

| | fps | thread de render | GPU parada no quadro |
|---|---|---|---|
| na fila (padrão agora) | **54,9 / 51,9** | 15,8 / 17,0 ms | **0,9 / 0,9 ms** |
| `LARECOMP_SLOT_DRAIN=1` | 43,7 / 44,7 | 18,0 / 17,5 ms | 5,0 / 4,9 ms |

Em média 53,4 × 44,2 fps (+21%). O lado na fila gasta mais watts porque faz mais quadros; por quadro fica igual
(~0,145 J).

**Primeira medição, na 0.4.6** (rodadas seguidas):

| | fps | render | GPU | GPU parada no quadro |
|---|---|---|---|---|
| esvaziando (padrão) | 45,4 | 18,4 ms | 18,7 ms | 3,4 ms |
| **na fila** | **52,4 (+15%)** | 17,8 ms | 18,9 ms | **0,15 ms** |

**Loops de inventário a 30 fps, na 0.4.6** (abre a Algibeira, troca de tela 3 vezes, fecha; 19 ciclos em 2 min, com
cooler):

| | quadros > 40 ms | render | GPU | potência |
|---|---|---|---|---|
| na fila | 0 | 8,09 ms | 14,67 ms | 2,38 W |
| esvaziando | 2 | 8,95 ms | 15,27 ms | 2,66 W |

**Sessão longa na fila:** 15 min de ciclos (149), 27.314 quadros a 30,0 fps, nenhum acima de 40 ms, potência estável
(2,11 W no 1º minuto, 2,04 W no 15º). Sem textura trocada ou faltando.

Na 0.4.7 o inventário ficou em 30,0 fps nos dois modos, sem textura trocada, mas a fila gastou um pouco **mais** (2,78 ×
2,44 W e 2,57 × 2,20 W), o contrário da 0.4.6. Causa não encontrada (ruído de rodadas curtas ou mudança da 0.4.7); por
isso o ganho que vale é o de 60 fps.

## 5. Modo 40 / 45 fps pela maior taxa que a tela oferece (`device.c`, JNI) - #455, mesclado

**Problema.** O HyperOS deixa um jogo em 60 Hz numa tela de 120 Hz até a janela pedir mais. O `vk_between_fps()`
decide pela taxa **atual**; com a tela em 60 Hz o modo saía 30, a janela não pedia nada e a tela nunca ia a 120 Hz.
O SDL registra só o modo atual (`SDL_androidvideo.c`), então a resposta não pode vir dele.

**A primeira versão do PR não tinha efeito no Android:** pegava a maior taxa pelo `SDL_GetFullscreenDisplayModes`, e o
SDL só registra o modo atual - a "maior taxa" era sempre a atual. O log de 40 fps que a justificava veio de uma rodada
com a tela já em 120 Hz.

**Como entrou** (refeito pelo JNI no mesmo PR). `BOTWRecompAndroidActivity.maxRefreshRate()` (a maior taxa de
`Display.getSupportedModes()` na resolução atual), lida uma vez por `plat_max_refresh_hz()`, decide o que o
`android_frame_rate` pede. A taxa do jogo continua seguindo a taxa atual da tela: fica em 30 até o sistema trocar, e num
sistema que não troca.

**Resultado (versão JNI).** Abrindo do zero com a tela em 60 Hz, a tela foi a 120 Hz e o jogo a 40,0 fps em 11 a 25 s,
em 4 de 5 rodadas. A que falhou foi com o celular quente: a tela ficou em 60 Hz e o jogo em 30, como antes (suspeita de
limite térmico do HyperOS, não confirmada).

Sustentado: 40 fps nos 4 primeiros minutos (5,3-5,6 W, SoC estabiliza em ~66 °C); depois o HyperOS limita a GPU a
629 MHz e fica em 38-39 fps.

**Conferido de novo na 0.4.13** (24/11, celular frio e na bateria, Frame rate 45): ao abrir, `the screen offers up to
120 Hz` e `the window asks for 40 fps content`; ainda no carregamento, `40 fps on a 120 Hz screen`. Em jogo, 2 min:
39,9 fps (25,07 ms de média, GPU 19,0 ms), 10 de 4.794 quadros acima de 40 ms.

## 6. Opção Vídeo > Sombras (`exec.c`, `settings`) - #457, mesclado

**Como entrou.** Ligada por padrão; desligada, pula os draws de passes só de profundidade em alvos fora do grupo de
escala da cena - os shadow maps. Critério só estrutural, sem hash de shader. Na 1.8.2 pega um único alvo, as cascatas do
sol (896x896, 3 camadas); nenhum pré-passe de profundidade nem passe de oclusão. Visual: a cena fica toda iluminada por
igual.

**Resultado** (A/B na mesma partida, 720p):

| sem × com sombras | fps | GPU | render | potência |
|---|---|---|---|---|
| 30 fps | 30 × 30 | −0,8 ms (−3,4%) | −1,2 ms (−5%) | igual |
| 60 fps (GPU no limite) | **59,3 × 55,4** | 15,5 × 18,3 ms | 13,3 × 17,0 ms | 6,66 × 6,84 W |

Na maior parte é uma escolha visual; compensa num aparelho no limite.

## Configurações que valeram neste aparelho

| Opção | Medido | Recomendação |
|---|---|---|
| `inplace_reads = 1` no Adreno 750 + Turnip | GPU −0,9 ms (−3,7%), −0,2 W; terreno sem listras (#129) com o Turnip Gen8 V37 | O Automático poderia ligar no Adreno 7xx com Turnip após mais testes |
| 720p × 1080p a 30 fps (A-B-A) | 1080p: GPU +3 ms, mas **+1,8 W (+41%) e +8 °C** (o clock da GPU sobe) | Resolução menor não muda o fps, mas muda muito o calor |
| Celular carregando × na bateria | SoC 76 °C × 55 °C na mesma configuração | Vale citar no README |

## O que não deu ganho (com o que já existe no app)

- **Turnip LRZ ligado** (`turnip_lrz`, Adreno 750): GPU 23,3 × 23,4 ms, 4,06 × 4,00 W - ruído.
- **`LARECOMP_UBO_DIRECT`**: −0,5 ms na thread de render, potência igual.
- **Cache de descriptor sets valendo o quadro inteiro**: com `LARECOMP_VK_CHUNK_US=0` (uma submissão por quadro) a taxa
  de acerto do cache ficou igual - 90,1% × 90,0%. As falhas não vêm das trocas de submissão; não vale mexer no tempo de
  vida dos pools.
- **Push descriptors (`VK_KHR_push_descriptor`)**, avaliado só pelo perfil: o `vkUpdateDescriptorSets` é ~1,6% da thread
  de render. O custo do `stage_set` (~25% da thread) está no nosso código: cópias de constant buffers (`guest_copy` +
  memcpy, ~10%), o hash dos writes, montar os arrays.
- **O "desenho mais caro" do `LARECOMP_GPUPROF_AT`**: um draw único em 1920x1080 (fs `90b79e85a3d120e3`, a cópia da
  cena para a imagem final) aparecia com 4,8 ms. Pulando ele (`LARECOMP_SKIP_FSHASH`) a GPU não mudou (24,2 × 24,5 ms):
  o perfil por passe soma ao passe a GPU parada até a marca seguinte. Para achar custo real, comparar o `gpu` do
  `LARECOMP_LOG_PACE` com e sem o draw, não a linha do perfil.

## Testado fora do repositório

Código que existiu só no build de teste do usuário, sem PR. **As variáveis e chaves A/B citadas aqui não existem no
app**; ficam como registro do que foi medido.

- **Apresentação numa thread própria** (`LARECOMP_PRESENT_THREAD=1` e chave A/B `present` no build de teste): o
  `vkQueuePresentKHR` sai da thread de render, com uma trava única para fila e swapchain e espera da apresentação
  pendente antes de acquire, recriação da swapchain e `vkDeviceWaitIdle`. A/B a cada 60 quadros: **render −1,2 ms
  (19,6 × 20,8)**, GPU igual, potência igual (4,22 × 4,10 W). Funciona, mas a 30 fps a thread de render não é o limite,
  e a 40 fps o limite é a GPU (~22 ms por quadro com o aparelho a ~61 °C).
- **Cascatas de sombra do sol** (`LARECOMP_SHADOW_CASCADES=<n>` no build de teste): pula as cascatas 896x896x3 a partir
  da camada n. A/B a cada 60 quadros, 30 fps, 720p: n = 2, GPU −0,6 ms; n = 1, −0,9 ms; n = 0, −1,1 ms; potência igual
  nos três. A opção Sombras (#457) cobre o caso útil.
- **Pedido de 30 fps de conteúdo à janela** (`android_frame_rate` também nos modos 30 / 20): a tela a 60 Hz economiza
  0,33 W contra 120 Hz, mas o HyperOS já põe o jogo em 60 Hz sozinho no modo 30; inofensivo, pode ajudar em outros
  sistemas.
- **Limite térmico pela GPU (`kgsl`)**: o HyperOS informa status térmico 0 mesmo com a GPU limitada a 578-629 de 903
  MHz, mas o SELinux não deixa um app ler `max_gpuclk` (`vendor_sysfs_kgsl`; só o `gpu_model` é liberado). A ideia não
  funciona sem outro sinal (por exemplo o tempo de GPU medido pelo próprio motor).
- **Fixar a thread de render nos núcleos rápidos**: o HyperOS (joyose/miperf) ignora `sched_setaffinity` - nem como
  root a máscara muda.

## Perfil da thread de render a 30 fps (para quem continuar)

`simpleperf record -t <render> --call-graph dwarf` e `report --symfs` com a `libmain.so` não strippada (o
`LARECOMP_VK_PROF` infla tudo neste aparelho: lê o relógio duas vezes por item, e o relógio aqui custa ~0,6 µs). Tempo
próprio dentro da `libmain.so`, medido antes do #452:

| Função | % |
|---|---|
| `draw_` (com o `stage_set` inline) | 22,8 |
| `guest_copy` | 7,5 |
| `vk_tex` (com `fnv_sample` / `hash_full` da checagem de textura: ~5) | 6,6 |
| `texture_info` | 6,2 |
| `exec_list` | 5,8 |
| `tex_backend` | 3,3 |
| `vk_image_view`, `hash_words`, `is_attachment_entry`, `vk_object_slot`, `begin_pass`, `widescreen_ui_centred`, `renderer_present_count` | 1-2 cada |

Perfil plano, como em `DESEMPENHO-60FPS-2026-10-23.md`. Fora da `libmain.so`: o driver Turnip (~24%) e a apresentação
(`queueBuffer`, ioctl do kgsl: ~6-7%). As alavancas grandes que sobram são estruturais: traduzir a lista em duas
threads (secondary command buffers) e, na GPU, menos draws (distância de desenho).
