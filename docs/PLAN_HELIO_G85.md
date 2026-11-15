# Implementation Plan: Helio G85 (Mali-G52 MC2) + 4GB RAM Android Support

**Projeto:** mcla-recomp-rv-helio-g85 (fork de gabrielbskk/mcla-recomp-rv)
**Alvo:** Dispositivos Helio G85 (Mali-G52 MC2, Bifrost), 4GB RAM, Android ARM64
**Compatibilidade:** Manter compatibilidade com repositório original (gabrielbskk/mcla-recomp-rv, Snapdragon/Adreno)
**Arquivo:** `docs/PLAN_HELIO_G85.md`
**Status:** Planejamento apenas — nenhuma alteração de código-fonte ainda.

---

## Overview

Este plano adapta o MCLA Recomp RV para rodar em dispositivos com GPU Mali-G52 MC2 (Helio G85) e 4GB de RAM no Android. O build atual foca Snapdragon/Adreno; o suporte a Mali foi descontinuado (docs/android-mali-driver.md) devido a dois problemas: (1) o driver stock Mali não expõe `vertexPipelineStoresAndAtomics`, fazendo `command_processor.cpp` rejeitar draws, e (2) o PanVK reporta a feature mas falha com `VK_ERROR_INVALID_EXTERNAL_HANDLE` na swapchain (WSI/gralloc). O plano introduz perfis de build para Mali, fallbacks de Vulkan, otimizações de memória para 4GB RAM e um cronograma em 3 fases, mantendo o build Snapdragon/Adreno inalterado e compatível com o upstream.

---

## Requirements

1. O APK deve compilar e instalar em um dispositivo Helio G85 / Mali-G52 MC2 com 4GB RAM, Android ARM64, API 23+.
2. O Vulkan deve inicializar com o driver stock Mali (sem `vertexPipelineStoresAndAtomics`) usando um fallback de compute shader para vertex-shader memory export, OU com PanVK se o WSI/gralloc puder ser contornado.
3. Resolução alvo: 1280x720 (720p) por padrão no perfil Mali, para economizar GPU e memória em 4GB RAM.
4. O build Snapdragon/Adreno existente deve continuar funcionando sem alterações de comportamento (compatibilidade com o upstream mantida).
5. Cache de pipeline, texturas e buffers devem ser otimizados para 4GB RAM (menor footprint, cache por driver, pipeline cache persistente).
6. Critérios de sucesso mensuráveis por fase, validáveis em dispositivo real.

---

## Architecture Changes

Abaixo, "fork" = árvore do projeto em `mcla-recomp-rv-helio-g85/`; "SDK" = ReXGlue v0.10.0 em `third_party/rexglue-sdk/` (não versionado no git, pinado por commit). O `command_processor.cpp` citado em docs/android-mali-driver.md vive no SDK, não no `src/` do fork.

| # | File path | Description |
|---|-----------|-------------|
| 1 | `docs/PLAN_HELIO_G85.md` | Este plano (criação). |
| 2 | `android/AndroidManifest.xml` | Adicionar `meta-data` `GPU_VARIANT` opcional `mali`; manter `snapdragon` como default. |
| 3 | `CMakeLists.txt` | Substituir o `FATAL_ERROR` do `MCLA_MALI_DIAGNOSTIC_APK` por um perfil Mali válido; adicionar flags de otimização para 4GB RAM e arquitetura ARMv8-A (cortex-a75). |
| 4 | `CMakePresets.json` | Adicionar preset `android-arm64-mali-release` (e debug) com `ANDROID=ON`, `REXGLUE_MALI_G85=ON`, `CMAKE_BUILD_TYPE=Release`, flags `-march=armv8-a`, `-mcpu=cortex-a75`. |
| 5 | `scripts/build_android_apk.sh` | Adicionar suporte a variante Mali via variável de ambiente `MCLA_GPU_VARIANT=mali` (afeta apenas nome do APK de saída e meta-data; bibliotecas nativas compartilhadas com o build Adreno). |
| 6 | `src/mcla_app.cpp` | `ApplyPcDefaults()`: adicionar defaults específicos Mali/4GB (MSAA off, sombras off, textura cache menor, `vulkan_pipeline_cache_file=true`, `mcla_render_height=720` já existe para Android). |
| 7 | `src/perf_options.cpp` | Adicionar preset `lowest` já existente; garantir que defaults Mali desliguem sombras e MSAA por padrão. |
| 8 | `src/render_perf.cpp` | Nenhuma mudança obrigatória (edram_tiles=4096 já é default Android); considerar reduzir para 2048 no perfil Mali se 4GB pressionar. |
| 9 | `src/mcla_app.h` | Declarar nova função/cvar se necessário para seleção de driver Mali (opcional, pode ser só manifest + cvar). |
| 10 | `third_party/rexglue-sdk/src/graphics/vulkan/command_processor.cpp` | **SDK.** Adicionar fallback quando `vertexPipelineStoresAndAtomics` não for suportado: redirecionar draws que exportam memória via vertex shader para um compute shader + buffer copy, em vez de rejeitar o draw. |
| 11 | `third_party/rexglue-sdk/src/ui/vulkan/` (presenter/swapchain) | **SDK.** Adicionar tentativa PanVK com fallback graceful: ao receber `VK_ERROR_INVALID_EXTERNAL_HANDLE` na criação da swapchain, tentar `VK_ANDROID_native_buffer` com gralloc alternativo, e por último cair para o driver system Vulkan. |
| 12 | `third_party/rexglue-sdk/src/system/gpu_plugin.cpp` (ou similar) | **SDK.** Expor cvar `vulkan_mali_fallback_mode` (auto/stock/compute/panvk/system) para escolher o caminho no runtime. |
| 13 | `patches/rexglue-android-arm64.patch` | Se o fallback de command_processor precisar de alterações no SDK, incluir neste patch (ou novo patch dedicado). |

---

## Implementation Steps

### Phase 1: Build compila (infraestrutura e perfis)

Objetivo: o APK Mali compila, é assinado, instala e inicia o launcher no Helio G85. Não há requisito de gameplay ainda.

1. **Criar `docs/PLAN_HELIO_G85.md`** (este arquivo).
   - Why: registro do plano e decisões.
   - Dependencies: Nenhuma.

2. **Atualizar `android/AndroidManifest.xml`** — adicionar suporte a variante Mali.
   - Action: Adicionar `android:uses-feature android:name="android.hardware.vulkan.version" android:required="false"`; manter `GPU_VARIANT=snapdragon` como default e documentar que um build separado define `GPU_VARIANT=mali`.
   - Why: permite distinguir builds Mali de builds Adreno no launcher.
   - Dependencies: Nenhuma.
   - Risk: Low.

3. **Atualizar `CMakeLists.txt`** — remover o bloqueio do build Mali e adicionar flags.
   - Action: Substituir o bloco `if(MCLA_MALI_DIAGNOSTIC_APK) ... message(FATAL_ERROR ...)` por um bloco que define `MCLA_GPU_VARIANT` (default `snapdragon`, aceita `mali`) e adiciona `add_compile_definitions(MCLA_GPU_VARIANT_${MCLA_GPU_VARIANT})`. Adicionar `-march=armv8-a -mcpu=cortex-a75 -O2 -DNDEBUG` para Android Release quando `MCLA_GPU_VARIANT STREQUAL mali`.
   - Why: permite o build Mali sem falhar e otimiza para a CPU do Helio G85 (2x Cortex-A75 + 6x Cortex-A55).
   - Dependencies: Nenhuma.
   - Risk: Low.

4. **Atualizar `CMakePresets.json`** — adicionar presets Android Mali.
   - Action: Adicionar `android-arm64-mali-debug` e `android-arm64-mali-release` herdando uma nova base `android-base` (ARM64, NDK, `-DCMAKE_ANDROID_ARCH_ABI=arm64-v8a`, `-DCMAKE_ANDROID_PLATFORM=android-34`, `-DCMAKE_POSITION_INDEPENDENT_CODE=ON`, `-DCMAKE_BUILD_TYPE=...`, `-DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=OFF -DREXGLUE_ENABLE_FSR=OFF`).
   - Why: builds reproduzíveis via `cmake --preset`.
   - Dependencies: Nenhuma.
   - Risk: Low.

5. **Atualizar `scripts/build_android_apk.sh`** — suporte a variante Mali.
   - Action: Ler `MCLA_GPU_VARIANT` (default `snapdragon`); usar como sufixo do APK de saída (ex: `MCLA-Recomp-mali-arm64.apk`) e gravar `GPU_VARIANT` no manifest via `aapt2` se for `mali`. Manter toda a cadeia de assinatura/zipalign inalterada.
   - Why: separa APKs Mali e Adreno sem alterar o keystore.
   - Dependencies: Passo 3.
   - Risk: Low.

6. **Verificar build de compilação (sem dispositivo)**.
   - Action: Rodar `cmake --preset android-arm64-mali-release` (ou configure manual) e `cmake --build` para confirmar que compila. O SDK não está no checkout; documentar que o build completo requer `third_party/rexglue-sdk/` pinado (docs/android-build.md).
   - Why: garantir que o plano é executável antes de passar para a Fase 2.
   - Dependencies: Passos 2–5.
   - Risk: Medium (depende do SDK estar disponível).

**Critério de sucesso Fase 1:** `cmake --build` produz `libmain.so` (arm64-v8a) para o perfil Mali sem erros; `scripts/build_android_apk.sh` gera um APK assinado que instala e abre o launcher em dispositivo Helio G85.

---

### Phase 2: Init Vulkan (driver e fallback)

Objetivo: o Vulkan inicializa no Mali-G52, seja via driver stock com fallback, seja via PanVK com WSI contornado, ou cai para o driver system.

7. **Pesquisar caminho do `command_processor.cpp` no SDK** (leitura apenas, não modificar ainda).
   - Action: Localizar `third_party/rexglue-sdk/src/graphics/vulkan/command_processor.cpp` (ou equivalente) e identificar o ponto que rejeita draws sem `vertexPipelineStoresAndAtomics`. Identificar a função de desenho de vertex shader com memory export.
   - Why: definir o local exato do fallback na Fase 3.
   - Dependencies: Fase 1 completa (SDK disponível).
   - Risk: Medium (SDK pode não estar presente no ambiente de planejamento).

8. **Projetar o fallback de `vertexPipelineStoresAndAtomics`** (especificação, sem código).
   - Action: Documentar o fluxo: quando o device não expor a feature, draws que exigem vertex-shader memory export são redirecionados para um compute shader que lê os dados do vertex buffer e escreve no buffer de destino, com sincronização (barreira de memória) antes do draw real. Alternativa mais simples (fallback de segurança): pular esses draws com log de aviso (degradação visual, não travamento).
   - Why: evitar que o driver stock Mali rejeite draws e termine a renderização.
   - Dependencies: Passo 7.
   - Risk: High (complexidade de sincronização Vulkan).

9. **Projetar a estratégia PanVK + WSI** (especificação, sem código).
   - Action: Documentar tentativas em ordem: (a) driver system Vulkan (padrão, mais estável); (b) PanVK com `VK_ANDROID_native_buffer` e gralloc; (c) PanVK com fallback de alocação de imagem via `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_ANDROID`. Ao receber `VK_ERROR_INVALID_EXTERNAL_HANDLE` em `panvk_android.c:182`, logar o erro e tentar o próximo caminho.
   - Why: PanVK reporta a feature mas falha na WSI; precisa de fallback ordenado.
   - Dependencies: Nenhuma (especificação).
   - Risk: High (PanVK no Android não validado).

10. **Adicionar cvar de seleção de caminho Vulkan no SDK** (especificação).
    - Action: Definir `vulkan_mali_path` com valores `auto` (tentar system, depois PanVK, depois stock+fallback), `stock`, `panvk`, `system`. Default `auto`.
    - Why: permitir que o usuário/testador force um caminho sem recompilar.
    - Dependientes: Passos 8–9.
    - Risk: Medium.

11. **Definir defaults de renderização para Mali/4GB em `src/mcla_app.cpp` (`ApplyPcDefaults`)**.
    - Action: Adicionar ao bloco `#if REX_PLATFORM_ANDROID` (ou novo bloco `#if defined(MCLA_GPU_VARIANT_MALI)`): `mcla_msaa=false`, `mcla_anti_aliasing="off"`, `mcla_shadows=false`, `mcla_race_shadows=false`, `mcla_fullscreen_blur=false`, `edram_tiles=2048` (reduzido de 4096), `mcla_render_height=720` (já existe, manter), `vulkan_pipeline_cache_file=true`, `vulkan_async_skip_incomplete_frames_max_ms=40` (já existe).
    - Why: 4GB RAM e Mali-G52 são limitados; reduzir uso de memória e carga de GPU.
    - Dependencies: Passo 3 (flag `MCLA_GPU_VARIANT_MALI`).
    - Risk: Low.

12. **Adicionar defaults de textura/texture cache para 4GB RAM** (especificação).
    - Action: Identificar no SDK os cvars de limite de cache de textura e vídeo (`texture_cache_limit`, `gpu_memory_pressure_percent`) e definir valores mais baixos para o perfil Mali (ex: pressão de memória em 75% em vez de 90%).
    - Why: evitar OOM em 4GB RAM.
    - Dependencies: Nenhuma (especificação; alteração no SDK).
    - Risk: Medium.

**Critério de sucesso Fase 2:** O Vulkan cria instância e device no Helio G85 (driver stock ou PanVK ou system), apresenta pelo menos um frame (tela preta ou menu aceitável) sem crash por `vertexPipelineStoresAndAtomics` ou `VK_ERROR_INVALID_EXTERNAL_HANDLE`. Logs mostram o caminho escolhido.

---

### Phase 3: Jogável (otimizações, estabilidade, validação)

Objetivo: o jogo chega ao gameplay (menus + corrida curta) com taxa de quadros aceitável e sem OOM em 4GB RAM.

13. **Implementar fallback de `command_processor.cpp` no SDK** (especificado no passo 8).
    - Action: No ponto de rejeição de draw sem `vertexPipelineStoresAndAtomics`, redirecionar para compute-shader fallback ou pular o draw com log. Implementar sincronização de memória adequada.
    - Why: desbloquear renderização no driver stock Mali.
    - Dependencies: Passos 7–8.
    - Risk: High.

14. **Implementar fallback de WSI/PanVK no SDK** (especificado no passo 9).
    - Action: Ao criar swapchain, capturar `VK_ERROR_INVALID_EXTERNAL_HANDLE` e tentar caminhos alternativos; expor cvar para escolher o driver.
    - Why: tornar o PanVK viável ou cair graciosamente para o system driver.
    - Dependencies: Passos 9–10.
    - Risk: High.

15. **Implementar otimizações de memória 4GB RAM** (especificadas no passo 12).
    - Action: Reduzir textura cache, EDRAM tiles, limite de pressão de GPU, desabilitar MSAA e sombras por padrão no perfil Mali.
    - Why: evitar OOM e manter FPS estável.
    - Dependencies: Passo 11.
    - Risk: Medium.

16. **Adicionar cvars de ajuste fino para Mali** (opcional).
    - Action: `mali_vertex_export_fallback` (bool), `mali_prefer_720p` (bool), `mali_disable_msaa` (bool).
    - Why: permitir que usuários testem diferentes combinações sem recompilar.
    - Dependencies: Passo 10.
    - Risk: Low.

17. **Testar em dispositivo Helio G85 real**.
    - Action: Instalar APK Fase 3, abrir launcher, selecionar driver, iniciar jogo, validar menus e uma corrida curta. Coletar logs de Vulkan, FPS e uso de memória.
    - Why: validação end-to-end.
    - Dependencies: Passos 13–16.
    - Risk: High (depende de dispositivo físico).

**Critério de sucesso Fase 3:** Jogo inicia, menus e pelo menos uma corrida curta rodam sem crash; FPS médio >= 20 (alvo 30) em 720p no Helio G85; sem OOM em 4GB RAM após 15 min de jogo; pipeline cache persistente reduz o tempo de criação de pipelines na segunda execução.

---

## Testing Strategy

### Unit tests (host, sem dispositivo)
- `scripts/ci/host_check.py`: verificar que o CMake configure não falha com o preset Mali.
- Verificar que `CMakeLists.txt` não emite `FATAL_ERROR` para `MCLA_GPU_VARIANT=mali`.

### Integration tests (dispositivo Helio G85)
- Instalar APK Mali e APK Adreno no mesmo dispositivo (pacotes distintos ou mesmo package com data separada — ver nota em android-mali-driver.md sobre package ID compartilhado).
- Abrir launcher, escolher driver (system / PanVK / stock), iniciar jogo.
- Coletar logs via `scripts/collect_android_logs.sh` e `adb logcat`.
- Validar criação de swapchain, apresentação de frames, ausência de `VK_ERROR_INVALID_EXTERNAL_HANDLE` e de rejeições de draw.

### E2E tests (dispositivo Helio G85)
- Jornada: launcher → importar game files (se ainda não importados) → PLAY → menu principal → iniciar corrida → dirigir por 5–15 min.
- Verificar FPS (overlay ou log), temperatura da GPU, uso de RAM (não deve se aproximar de 3.5GB dos 4GB totais).
- Segunda execução: verificar que o pipeline cache persistente é carregado e reduz o tempo de criação de pipelines.

### Edge cases / error scenarios
- Driver stock sem `vertexPipelineStoresAndAtomics`: draw rejeitado → fallback ativado, jogo continua (possivelmente com artefato visual sem crash).
- PanVK `VK_ERROR_INVALID_EXTERNAL_HANDLE`: fallback para system driver, jogo continua.
- Baixa memória (4GB, outros apps abertos): reduzir cache de textura, não crashar por OOM.
- Device sem PanVK instalado: usar driver system ou stock + fallback.

---

## Risks & Mitigations

| Risk | Description | Mitigation |
|------|-------------|------------|
| High | `vertexPipelineStoresAndAtomics` ausente no driver stock Mali exige fallback de compute shader no SDK, que é complexo e pode ter bugs de sincronização. | Começar pelo fallback mais simples (pular draw com log) para validar que o jogo não trava; depois implementar compute fallback. |
| High | PanVK falha com `VK_ERROR_INVALID_EXTERNAL_HANDLE` na swapchain (WSI/gralloc) e pode não ser solucionável no escopo deste fork. | Manter o driver system Vulkan como caminho padrão e confiável; PanVK como opcional, desabilitado por padrão. |
| Medium | O SDK (`third_party/rexglue-sdk/`) não está versionado no git; mudanças no `command_processor.cpp` dependem do SDK estar disponível e ser recompilar. | Documentar exatamente qual commit do SDK é necessário e onde aplicar os patches; não modificar o SDK sem testar o build Adreno para não quebrar compatibilidade. |
| Medium | 4GB RAM é insuficiente para cache de pipeline grande + texturas + jogo em execução. | Reduzir EDRAM tiles, desabilitar MSAA/sombras, limitar textura cache, usar pipeline cache persistente por driver. |
| Medium | O package ID `org.mcla.recomp` é compartilhado entre builds Adreno e Mali (docs/android-mali-driver.md); instalar um substitui o outro e perde dados privados. | Usar package IDs distintos para builds Mali (ex: `org.mcla.recomp.mali`) ou documentar claramente que não se deve instalar ambos no mesmo dispositivo sem migração de dados. |
| Low | Helio G85 tem CPU limitada (2x A75 + 6x A55); a recompilação estática do Xbox 360 é pesada para a CPU. | Definir `vulkan_async_skip_incomplete_frames_max_ms=40`, `mcla_poll_wait` apropriado, reduzir resolução para 720p, desabilitar pós-processamento pesado. |
| Low | Não há dispositivo Helio G85 disponível no ambiente de planejamento para validação. | Marcar critérios que exigem dispositivo físico como "pendente de validação em hardware real"; o plano pode ser executado parcialmente em emulador/Termux sem o dispositivo final. |

---

## Success Criteria

### Fase 1 — Build compila
- [ ] `cmake --preset android-arm64-mali-release` configura sem erros.
- [ ] `cmake --build` produz `libmain.so` arm64-v8a para o perfil Mali.
- [ ] `scripts/build_android_apk.sh` gera APK assinado e zipalignado.
- [ ] O APK instala e abre o launcher em dispositivo Helio G85 (sem crash na abertura).

### Fase 2 — Init Vulkan
- [ ] Vulkan instancia e cria device no Helio G85 (qualquer caminho: system, stock+fallback ou PanVK).
- [ ] Pelo menos um frame é apresentado (tela do loader/menu visível).
- [ ] Nenhum crash por `vertexPipelineStoresAndAtomics` ausente ou `VK_ERROR_INVALID_EXTERNAL_HANDLE`.
- [ ] Logs mostram o caminho Vulkan escolhido e o cvar `vulkan_mali_path` funciona.

### Fase 3 — Jogável
- [ ] Menus e pelo menos uma corrida curta rodam sem crash.
- [ ] FPS médio >= 20 (alvo 30) em 1280x720 no Helio G85.
- [ ] Uso de RAM permanece abaixo de ~3.5GB dos 4GB totais (sem OOM).
- [ ] Pipeline cache persistente reduz tempo de criação de pipelines na segunda execução.
- [ ] O build Adreno/Snapdragon continua inalterado (regressão: build presets existentes ainda funcionam).

---

## Notes on Compatibility with Upstream

- O `src/` do fork é plano (sem subpasta `graphics/vulkan/`); o `command_processor.cpp` e o código Vulkan de swapchain vivem no **SDK ReXGlue** (`third_party/rexglue-sdk/`), não no fork. Todas as mudanças de renderer Vulkan devem ser feitas no SDK e aplicadas via patches (`patches/`), mantendo o `src/` do fork focado em integração Android (launcher, cvars, defaults).
- O `CMakePresets.json` não tem presets Android hoje; os novos presets Mali são adicionitivos e não removem os existentes.
- O `AndroidManifest.xml` mantém `GPU_VARIANT=snapdragon` como default para não alterar o comportamento dos builds existentes.
- Nenhum arquivo do `docs/fork-base/` (upstream) é modificado.
