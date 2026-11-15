# ARCH_HELIO_G85 — Decisões de Arquitetura: Adaptações para Helio G85 (Mali-G52 MC2, 4 GB RAM, Android)

## Metadados

| Campo | Valor |
|-------|-------|
| Projeto | MCLA Recomp RV — fork `Rydev-pc/mcla-recomp-rv-helio-g85` |
| Upstream | `gabrielbskk/mcla-recomp-rv` |
| Alvo | Helio G85 · Mali-G52 MC2 · 4 GB RAM · Android ARM64 |
| Status | Proposto (pendente de validação em dispositivo) |
| Data | 2026-11-15 |
| Autor | Arquiteto de Software (subagente OpenCode) |
| ADRs referenciados | ADR-001 … ADR-006 (abaixo) |

---

## Sumário Executivo

O Helio G85 (Mali-G52 MC2) é um alvo de baixo custo com duas restrições duras identificadas no diagnóstico arquivado (`docs/android-mali-driver.md`):

1. O driver stock Mali **não expõe** `vertexPipelineStoresAndAtomics`, exigido pelo renderizador Xenos atual.
2. O PanVK testado (Mesa 26.3.0-devel) **falha na criação do swapchain** no Android com `VK_ERROR_INVALID_EXTERNAL_HANDLE` (WSI/gralloc external-handle).

O projeto retirou o suporte Mali (`MCLA_MALI_DIAGNOSTIC_APK` gera `FATAL_ERROR` no CMake) e não mantém roadmap de PanVK. Este documento define como reintroduzir, de forma controlada e reversível, o suporte ao Helio G85 sem comprometer o build Snapdragon/Adreno existente nem o vínculo com o upstream.

As seis decisões (ADR-001 a ADR-006) cobrem: estratégia de renderização, local do feature-gate de `vertexPipelineStoresAndAtomics`, abstração de memória para 4 GB, presets de build CMake, seção do launcher e estratégia de compatibilidade com o upstream.

---

## 1. Current State Analysis (Estado Atual)

### 1.1. Código-fonte e build
- Linguagem: C++23, compilador clang/llvm-mingw.
- Renderização: SDL3 + Vulkan via ReXGlue (SDK v0.10.0, commit `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`).
- Recompilação estática: Xbox 360 (Xenos) → Android ARM64, código gerado em `generated/default`.
- `CMakeLists.txt`: monta `mcla` como shared library `libmain.so` no Android; rejeita `MCLA_MALI_DIAGNOSTIC_APK`.
- `CMakePresets.json`: presets para Windows/Linux/macOS (amd64/arm64). **Não há presets Android.**
- Patches Android ativos em `patches/` aplicados sobre o ReXGlue (turnip-loader, driver-cache-path, pipeline background work, etc.).

### 1.2. Fontes de verdade sobre Mali
- `docs/android-mali-driver.md`: suporte Mali descontinuado; stock driver sem `vertexPipelineStoresAndAtomics`; PanVK com `VK_ERROR_INVALID_EXTERNAL_HANDLE` em `panvk_android.c:182`.
- `docs/android-compatibility.md`: nenhuma RAM/SoC mínima estabelecida; S23 (8 GB) é o dispositivo de referência; 6 GB relatou lentidão em builds antigos.
- `command_processor.cpp` **não está em `src/`** do mcla: pertence ao SDK ReXGlue (renderizador Xenos/Vulkan). O feature-gate de `vertexPipelineStoresAndAtomics` deve, portanto, ser modelado como uma camada de abstração no lado do mcla que consulta capabilities do dispositivo antes de chamar o SDK.

### 1.3. Organização de branches
- `main` (origin) = branch de trabalho do fork.
- `upstream/main` = upstream `gabrielbskk/mcla-recomp-rv`.
- Patches Android já existem em `patches/` (receita do SDK ReXGlue).

---

## 2. Requirements (Requisitos)

### Requisitos Funcionais
- RF-01: O launcher Android deve oferecer um perfil "Helio G85 / Mali / Low RAM" selecionável pelo usuário.
- RF-02: Sob esse perfil, o app deve preferir o driver stock Mali com degradação funcional (fallback) quando `vertexPipelineStoresAndAtomics` estiver ausente.
- RF-03: O uso de memória deve caber em 4 GB RAM (sistema compartilhado GPU/CPU no Android), com folga para o SO e o jogo importado (~6.2 GB de cópia privada + caches).
- RF-04: O build CI deve produzir um APK android-arm64-helio-g85 sem alterar os APKs Snapdragon/Adreno existentes.
- RF-05: Mudanças para Helio G85 devem permanecer isoladas do upstream para não regressar o build principal.

### Requisitos Não-Funcionais
- RNF-01 (Performance): alvo jogável (métrica a definir por tester), não apenas swapchain criada.
- RNF-02 (Estabilidade): sem crash por falta de feature Vulkan; fallback silencioso ou erro guiado ao usuário.
- RNF-03 (Manutenibilidade): patches separados, aplicáveis em ordem, com pin de SHA-256 (modelo já usado em `scripts/ci/check_pins.py`).
- RNF-04 (Tamanho do binário): otimizações de tamanho (`-Os`, `-ffunction-sections`, `-fdata-sections`, `--gc-sections`, strip de símbolos).
- RNF-05 (Observabilidade): logs de qual driver foi selecionado, por que o fallback ocorreu, uso de memória e cache de pipelines.

---

## Decisões Arquiteturais (ADRs)

---

## ADR-001 — Estratégia de renderização: stock Mali com fallback vs PanVK

### Contexto
O Helio G85 tem dois caminhos Vulkan possíveis:

- **Stock Mali (driver Arm/vendor)**: cria swapchain (1600×720 relatado) e apresenta quadros, mas não expõe `vertexPipelineStoresAndAtomics`. Sem esse recurso, o renderizador Xenos rejeita draws de exportação de memória de vertex shader.
- **PanVK (Mesa Vulkan sobre Panfrost)**: expõe `vertexPipelineStoresAndAtomics`, mas o build Android testado falha no swapchain com `VK_ERROR_INVALID_EXTERNAL_HANDLE` (`panvk_android.c:182`), um problema de WSI/gralloc external-handle que depende do kernel, gralloc e loader do dispositivo específico.

### Decisão
Priorizar o **caminho do driver stock Mali com fallback funcional**, tratando o PanVK como **opcional, não-default e desligado por padrão**.

O perfil "Helio G85 / Mali / Low RAM" fará:
1. Detecção de GPU (Mali-G52) e de RAM (≤ 4 GB) no launcher.
2. Carregamento do driver stock Mali como Vulkan default.
3. Consulta de `VkPhysicalDeviceFeatures::vertexPipelineStoresAndAtomics`.
4. Se ausente: ativar caminho de fallback (descrição em ADR-002) em vez de abortar.
5. PanVK disponível apenas como override manual/oculto (variável de ambiente ou build debug), nunca como default, até que o erro `VK_ERROR_INVALID_EXTERNAL_HANDLE` seja reproduzido e corrigido no dispositivo alvo.

### Trade-offs

| Opção | Prós | Contras | Alternativas consideradas | Decisão |
|-------|------|---------|---------------------------|---------|
| **Stock Mali + fallback** (escolhido) | Swapchain já funciona no dispositivo; menor risco de integração WSI; reversível; não depende de build Mesa custom. | Requer implementar fallback de exportação de vertex memory (ex.: compute dispatch + sincronização) para draws que precisam do recurso faltante. | PanVK default; desabilitar recurso e aceitar draws faltantes. | Escolhido porque apresenta quadros hoje; o fallback é um problema de render isolado, enquanto PanVK não apresenta swapchain. |
| PanVK default | `vertexPipelineStoresAndAtomics` nativo; sem fallback de shader. | `VK_ERROR_INVALID_EXTERNAL_HANDLE` bloqueia swapchain no Android testado; dependências de gralloc/kernel/loader não controladas pelo projeto. | Construir PanVK NDK com flags Mesa documentadas. | Rejeitado como default: falha de integração de plataforma, não de shader. Pode ser revisado após validação do WSI. |
| Desabilitar `vertexPipelineStoresAndAtomics` sem fallback | Simples de implementar. | Draws de exportação de vertex memory simplesmente não ocorrem → artefatos visuais, possivelmente jogabilidade quebrada. | Rejeitado: não é degradação graciosa, é perda de função. | Rejeitado. |

### Consequências
- Positivo: caminho funcional imediato no stock Mali; isolamento do problema PanVK; alinha com o fato de o projeto já ter retirado Mali por falta de validação de gameplay.
- Negativo: exige engenharia de fallback de render; performance do fallback pode ser inferior no Mali de 2 núcleos.

---

## ADR-002 — Onde inserir o feature-gate de `vertexPipelineStoresAndAtomics`

### Contexto
O recurso é consultado em nível de `VkPhysicalDeviceFeatures`. No diagnóstico, `command_processor.cpp` (SDK ReXGlue, renderizador Xenos) rejeita draws de exportação de vertex shader sem o recurso. Como `command_processor.cpp` vive no SDK ReXGlue (não no `src/` do mcla), o mcla não deve editar o SDK diretamente; deve expor uma camada de decisão no próprio código do mcla que:
1. Consulta capabilities do dispositivo no momento da criação do dispositivo Vulkan.
2. Expõe um booleano `supportsVertexPipelineStoresAndAtomics` para os subsistemas que o consomem.
3. Controla a criação de pipelines e a exportação de shaders com base nesse booleano.

### Decisão
Inserir o feature-gate em **três camadas**, com responsabilidades separadas:

| Camada | Local (lógica) | Responsabilidade |
|--------|----------------|------------------|
| **Capability probe** | Inicialização do dispositivo Vulkan (wrapper mcla, ex.: `mcla_app.cpp` / novo módulo `gpu_capabilities.cpp`) | Chama `vkGetPhysicalDeviceFeatures`, armazena `vertexPipelineStoresAndAtomics` e expõe via getter. Não edita o SDK. |
| **Command processor gate** | `command_processor.cpp` do SDK (via patch separado `rexglue-helio-g85-vertex-export.patch`) | Recebe o booleano via callback/estrutura de configuração do ReXGlue; se `false`, rejeita ou redireciona draws de exportação de vertex memory para o caminho de fallback (compute dispatch), ao invés de abortar duro. |
| **Pipeline creation** | Criação de pipelines Vulkan no mcla (wrapper de `vkCreateGraphicsPipelines`) | Se o recurso estiver ausente, não habilita `VkPipelineVertexInputDivisorState`-like ou entradas de vertex que dependam de stores/atomics; usa pipeline alternativa. |
| **Shader export** | Preparação/exportação de shaders (recompilado Xenos → SPIR-V/GLSL via ReXGlue) | Se o recurso estiver ausente, emite shaders que evitam `imageStore`/`atomicAdd` em buffers de vertex, ou gera um shader de fallback (compute) para a mesma operação. |

O fluxo de decisão é: **capability probe → gate central → command processor / pipeline / shader export**. Nenhum desses componentes toma decisão sozinho; todos consultam o booleano central.

### Trade-offs

| Abordagem | Prós | Contras | Alternativas | Decisão |
|-----------|------|---------|--------------|---------|
| **Gate central em 3 camadas** (escolhido) | Separa preocupações; testável; não modifica o SDK sem motivo; fallback localizado. | Mais camadas para manter; requer coordenação entre mcla e patches do SDK. | Editar diretamente `command_processor.cpp` no SDK sem camada mcla. | Escolhido por manter o SDK como dependência externa e isolar a política de dispositivo no mcla. |
| Gate apenas no command_processor | Simples. | Não cobre criação de pipeline nem exportação de shader; risco de pipeline inválido ser criado. | Rejeitado. |
| Gate apenas na criação de pipeline | Pipelines válidos. | Shader e command processor ainda podem tentar operações não suportadas. | Rejeitado. |

### Consequências
- Positivo: falha controlada (fallback) em vez de crash; comportamento previsível em dispositivos Mali sem o recurso.
- Negativo: o caminho de fallback (compute dispatch + sincronização) precisa ser implementado e validado; pode impactar FPS no Mali-G52 MC2.

---

## ADR-003 — Abstração de memória: reduzir uso para 4 GB RAM

### Contexto
Android com 4 GB RAM compartilha memória entre CPU e GPU (UMA). O jogo importado ocupa ~6.2 GB de cópia privada (armazenamento, não RAM), mas caches de shader, pipeline cache, texturas e buffers de render consomem RAM em tempo de execução. Não há RAM mínima estabelecida no projeto; 6 GB relatou lentidão em builds antigos.

### Decisão
Introduzir uma **abstração de orçamento de memória** (`MemoryBudget` / `LowRAMProfile`) ativada pelo perfil Helio G85, com os seguintes limites e mecanismos:

| Recurso | Comportamento padrão (Adreno) | Perfil Helio G85 (4 GB) |
|---------|-------------------------------|--------------------------|
| Pipeline cache | Tamanho padrão do SDK/driver | Limite inferior (ex.: 16–32 MB), persistência em disco desabilitada ou cache menor, escrita em segundo plano (já existe `rexglue-vulkan-pipeline-background-work.patch`). |
| Texture limits | Mipmap completo, filtragem anisotrópica padrão | Limite de resolução de textura (cap, ex.: 1024 ou 2048), desabilitar anisotropia alta, usar formatos comprimidos (ASTC/ETC2 quando disponíveis). |
| Buffer pooling | Alocação sob demanda | Pool de buffers de comando e uniforme pré-alocado, reutilização de buffers de staging, limitar número de frames em voo (2 em vez de 3). |
| Shader cache | Cache completo | Cache limitado por título, evicção LRU, não compilar shaders não visíveis (lazy compilation). |
| Heap total | Sem orçamento rígido | Orçamento de RAM alvo (ex.: ≤ 1.5–2 GB para o processo do jogo), com log de alerta se excedido. |

Implementar como **uma única camada de abstração** (`MemoryBudget`) consultada no launcher e no inicializador Vulkan, não espalhada pelo código.

### Trade-offs

| Abordagem | Prós | Contras | Alternativas | Decisão |
|-----------|------|---------|--------------|---------|
| **Abstração centralizada `MemoryBudget`** (escolhida) | Consistente, fácil de ajustar, testável, afeta todos os alocadores por um único ponto. | Requer que alocadores existentes respeitem o orçamento. | Sem orçamento; deixar o driver gerenciar. | Escolhida porque 4 GB é uma restrição dura; deixar ao driver é imprevisível. |
| Orçamento por recurso (textura, pipeline, buffer separados) | Granularidade fina. | Complexo; risco de orçamentos conflitantes. | Rejeitado como ponto de partida. |

### Consequências
- Positivo: previsibilidade de RAM; menor risco de kill pelo OOM killer do Android; reutiliza infraestrutura existente de pipeline background work.
- Negativo: qualidade visual reduzida (texturas menores, menos anisotropia); mais código de gerenciamento de pool.

---

## ADR-004 — Estrutura de build: presets CMake para android-arm64-helio-g85

### Contexto
`CMakePresets.json` não tem presets Android. O build Android atual usa `scripts/build_android_apk.sh` e targets SNAPDRONG/Adreno. O alvo Helio G85 precisa de:
- Toolchain Android NDK (clang, arm64-v8a).
- `-march=armv8-a` (CPU Cortex-A75 do Helio G85; sem otimizações específicas de núcleo mais novo).
- Otimizações de tamanho (`-Os`, `-ffunction-sections -fdata-sections`, linker `--gc-sections`, strip).
- Perfil de runtime (stock Mali, fallback, low RAM).

### Decisão
Adicionar a `CMakePresets.json`:
- Um **preset base oculto** `android-arm64-base` (herda de um novo `android-base` que define `CMAKE_SYSTEM_NAME=Android`, `CMAKE_SYSTEM_VERSION`, `CMAKE_ANDROID_ARCH_ABI=arm64-v8a`, `CMAKE_ANDROID_NDK`, `CMAKE_ANDROID_STL`, `CMAKE_POSITION_INDEPENDENT_CODE=ON`, `CMAKE_CXX_FLAGS` iniciais).
- Presets de build concretos:
  - `android-arm64-helio-g85-debug`
  - `android-arm64-helio-g85-release` (Recommended)
  - `android-arm64-helio-g85-relwithdebinfo`
- Cada um herda `android-arm64-base` e define:
  - `CMAKE_C_FLAGS` / `CMAKE_CXX_FLAGS`: `-march=armv8-a -Os` (release) ou `-g` (debug).
  - `CMAKE_BUILD_TYPE`.
  - `MCLA_TARGET_PROFILE=helio-g85` (variável CMake que ativa os ADRs 001–003 em tempo de compilação, se desejado).
- Build preset correspondente em `buildPresets`.

Manter o script `scripts/build_android_apk.sh` como orquestrador, adicionando uma variante `--profile helio-g85` que seleciona o preset correto e assina o APK com um package ID distinto (ver ADR-006 sobre package ID).

### Trade-offs

| Abordagem | Prós | Contras | Alternativas | Decisão |
|-----------|------|---------|--------------|---------|
| **Presets CMake dedicados** (escolhidos) | Reprodutíveis, documentados, integrados ao VS Code/CMake, fácil CI. | Mais entrada em `CMakePresets.json`; requer NDK configurado. | Apenas flags no script shell. | Escolhidos por padronizar com o restante do projeto. |
| Reutilizar preset `linux-arm64` com cross-compile | Menos duplicação. | Não cross-compila para Android nativamente; faltam flags de plataforma. | Rejeitado. |

### Consequências
- Positivo: build isolado para Helio G85; flags de tamanho reduzem o APK; `-march=armv8-a` gera código seguro para Cortex-A75.
- Negativo: manutenção de mais presets; necessidade de NDK no CI.

---

## ADR-005 — Interface do usuário: seção no launcher para perfil "Helio G85 / Mali / Low RAM"

### Contexto
O launcher Android (`MclaLauncherActivity.java`, `MclaBuildVariant.java`) já lida com variantes de build. Não há atualmente um perfil Mali/Low RAM (Mali foi retirado).

### Decisão
Adicionar ao launcher uma **seção de seleção de perfil de dispositivo**, visível apenas quando o build for a variante `helio-g85` (ou quando detetado dispositivo de baixo specs), contendo:

- Radio/seleção: "Helio G85 / Mali-G52 / Low RAM (4 GB)" vs "Padrão (Snapdragon/Adreno)".
- Ao selecionar Helio G85:
  - Ativar `LowRAMProfile` (ADR-003).
  - Forçar driver stock Mali (ADR-001).
  - Avisar o usuário (dialog único) de que a experiência é experimental, que faltam recursos do driver stock e que o PanVK não é suportado neste build.
- Exibir no rodapé/debug overlay: driver Vulkan selecionado, presença/ausência de `vertexPipelineStoresAndAtomics`, uso de RAM aproximado.

A seleção deve ser persistida em `SharedPreferences` e lida na inicialização do Vulkan.

### Trade-offs

| Abordagem | Prós | Contras | Alternativas | Decisão |
|-----------|------|---------|--------------|---------|
| **Seção no launcher nativo Java** (escolhida) | Integra com o fluxo existente do app; persistência via `SharedPreferences`; visível ao usuário final. | Requer tocar em código Java do launcher. | Somente variável de ambiente/build-time. | Escolhida porque o usuário precisa escolher e ser informado; build-time não é suficiente. |
| Detecção automática por modelo de SoC | Sem intervenção do usuário. | Frágil (nomes de modelo variam; não detecta RAM confiavelmente em todos os Android). | Complementar, não substituto. | Usar detecção como sugestão, não como escolha única. |

### Consequências
- Positivo: transparência para o usuário; evita que usuários de Adreno sejam afetados pelo perfil Mali.
- Negativo: superfície de UI adicional para manter.

---

## ADR-006 — Compatibilidade com o repositório upstream (`gabrielbskk/mcla-recomp-rv`)

### Contexto
O fork tem `upstream` apontando para `gabrielbskk/mcla-recomp-rv` e `origin` para `Rydev-pc/mcla-recomp-rv-helio-g85`. O upstream não tem suporte Mali/Helio G85 (de fato, o fork retirou o diagnóstico Mali). Mudanças do fork não devem poluir o upstream nem regressar o build principal.

### Decisão
Manter as adaptações Helio G85 **isoladas do upstream** da seguinte forma:

1. **Branch dedicado**: criar branch `helio-g85` (ou `feature/helio-g85`) a partir de `main` do fork. Todo o trabalho de Helio G85 fica nessa branch.
2. **Patches separados**: criar um patch dedicado `rexglue-helio-g85-vertex-export.patch` (ou similar) para o feature-gate no SDK, separado dos patches Android existentes em `patches/`. Manter o pin de SHA-256 em `scripts/ci/check_pins.py`.
3. **Presets e build**: os presets `android-arm64-helio-g85-*` só existem no fork; não abrir PR para o upstream.
4. **Launcher**: a seção de perfil Helio G85 só é compilada no build `helio-g85` (flag CMake `MCLA_TARGET_PROFILE=helio-g85`), não no build default Adreno.
5. **Sincronização com upstream**: quando o upstream avançar, fazer `git fetch upstream` e rebase/merge do `upstream/main` para `main` do fork, resolvendo conflitos na branch `helio-g85` separadamente. Nunca fazer push da branch `helio-g85` para o upstream.

### Trade-offs

| Abordagem | Prós | Contras | Alternativas | Decisão |
|-----------|------|---------|--------------|---------|
| **Branch + patches separados** (escolhida) | Isolamento total do upstream; fácil rebase; não regressa o build principal. | Duplicação de manutenção (build Mali vs build Adreno). | Mesclar tudo no `main` do fork e tentar PR no upstream. | Escolhida porque o upstream já retirou Mali; não faz sentido propor suporte Mali lá. |
| Fork permanente separado | Isolamento máximo. | Perde rastreamento fácil com upstream. | Rejeitado: o fork já existe e quer manter vínculo com upstream. |

### Consequências
- Positivo: upstream protegido; CI do Adreno não afetada; rastreamento de patches por SHA-256.
- Negativo: necessidade de manter a branch `helio-g85` atualizada com o `main` do fork manualmente.

---

## 3. Design Proposal (Resumo da Arquitetura)

```
Launcher (Java)
  └─ Perfil selecionado: "Helio G85 / Mali / Low RAM"
        ├─ SharedPreferences: persiste escolha
        ├─ Ativa LowRAMProfile (MemoryBudget)
        └─ Força driver stock Mali

CMake (android-arm64-helio-g85-release)
  ├─ -march=armv8-a -Os
  ├─ MCLA_TARGET_PROFILE=helio-g85
  ├─ Pipeline cache limitado, escrita em segundo plano
  ├─ Texture limits, pool de buffers, frames-in-voo=2
  └─ Builds libmain.so + APK com package ID distinto

Vulkan init (mcla)
  ├─ vkGetPhysicalDeviceFeatures → vertexPipelineStoresAndAtomics
  ├─ Booleano central exposto para:
  │    ├─ command_processor (patch SDK) → fallback compute dispatch se ausente
  │    ├─ pipeline creation → pipeline alternativa se ausente
  │    └─ shader export → shader sem imageStore/atomic em vertex se ausente
  └─ Log: driver, feature presente/ausente, RAM aproximada
```

---

## 4. Plano de Implementação (Fases)

Fase 1 — Build e detecção: presets CMake `android-arm64-helio-g85-*`, flag `MCLA_TARGET_PROFILE`, detecção de GPU/RAM, logs.
Fase 2 — Feature-gate: patch do SDK para `command_processor.cpp`, gate central, pipeline e shader export alternativos (sem código de fallback funcional ainda, apenas desabilitação graciosa).
Fase 3 — Memória: `MemoryBudget` com limites de pipeline cache, texturas, buffer pooling, frames-in-voo.
Fase 4 — UI: seção no launcher, diálogo de aviso, debug overlay.
Fase 5 — Validação: APK assinado com package ID distinto, teste em Helio G85, medição de FPS e RAM, decisão sobre PanVK (revisar ADR-001 se WSI for corrigido).

---

## 5. Riscos e Mitigações

| Risco | Probabilidade | Impacto | Mitigação |
|-------|---------------|---------|-----------|
| Fallback de vertex memory lento ou quebrado no Mali-G52 MC2 | Alta | Médio (jogabilidade afetada) | Implementar fallback como compute dispatch; medir FPS; se inviável, desabilitar draws afetados com aviso ao usuário. |
| PanVK WSI nunca funcionar no dispositivo | Média | Baixo (se ADR-001 priorizar stock) | Manter PanVK como override oculto; não bloquear o lançamento do perfil stock. |
| Regressão no build Adreno por flags compartilhadas | Baixa | Alto | Presets e flags isolados por `MCLA_TARGET_PROFILE`; CI separada para Adreno. |
| OOM killer em 4 GB apesar do orçamento | Média | Alto | Orçamento conservador; logs de RAM; avisar usuário a fechar apps em segundo plano. |
| Divergência da branch `helio-g85` do `main` do fork | Média | Médio | Rebase regular; CI de build da branch helio-g85 a cada merge no `main`. |

---

## 6. Referências

- `docs/android-mali-driver.md` — diagnóstico arquivado do Mali-G52 / Helio G85.
- `docs/android-compatibility.md` — requisitos e incertezas de RAM/SoC.
- `patches/README.md` — receita de patches Android e pin de SHA-256.
- `scripts/ci/check_pins.py` — validação de pins de patches.
- Mesa PanVK docs: https://docs.mesa3d.org/drivers/panfrost.html, https://docs.mesa3d.org/android.html
- Android Vulkan: https://source.android.com/docs/core/graphics/implement-vulkan

---

## Apêndice — Modelo de registro de decisão (ADR) usado

Cada decisão acima segue o formato:

```
## ADR-NNN — Título

### Contexto
...

### Decisão
...

### Trade-offs
| Opção | Prós | Contras | Alternativas consideradas | Decisão |

### Consequências
- Positivo: ...
- Negativo: ...
```

Quando uma decisão for implementada, este documento deve ser atualizado com o status (Aceito/Implementado/Deprecado) e a data.
