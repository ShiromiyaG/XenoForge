# XenoForge — instruções para o Claude

**Emulador AoT de Xbox 360 com SDK para ports** (objetivo revisto com o dono em 2026-11-05): o launcher
recompila o disco do jogador na máquina dele numa biblioteca do jogo em cache, e o player genérico a roda; o
SDK serve ports de jogos específicos como o rexglue, mas mais fácil de depurar (achar o erro é um recurso:
relatórios com a pilha do guest, interpretador conferindo o código gerado, replays exatos, oráculo para cada
parte). Escrito do zero com o Xenia como referência; kernel e GPU completados por inventário (famílias
inteiras portadas do Xenia de uma vez, `docs/plan.md` "Everything left"), não só o que os jogos pedem. Na GPU,
onde o Xenia não tem resposta, o XNA Game Studio 4.0 (`tools/xna/`) é a segunda referência. Plano:
`docs/plan.md` (**fora do git, só local**, pedido do dono em 2026-11-06: está no `.gitignore`; worktrees não o têm,
então agentes o leem em `F:\RECOMPs\XenoForge\docs\plan.md`, não o editam e trazem as mudanças no relatório, que eu
aplico); renderizador: `docs/renderer.md`; o que os jogos importam: `docs/lean-kernel.md`.

- Remoto: `origin` = https://github.com/Tsukimi-Labs/XenoForge (**privado**; criado em 2026-11-06 como
  `ShiromiyaG/XenoForge`, transferido para a organização Tsukimi Labs em 2026-11-12, branch
  `master`). Commits locais de rotina permitidos; **push só quando o dono pedir**. Só o `master` vai para o
  GitHub (nunca os branches `worktree-agent-*`). Docs, comentários, logs e commits em inglês; só este arquivo
  em português.
- **No máximo 4 agentes rodando ao mesmo tempo** (pedido do dono, 2026-11-06: oito em paralelo estouraram o
  limite de uso de 5 horas). Novos agentes só quando os anteriores terminarem. Agentes `xf-*`
  (`.claude/agents`), em worktrees; builds com `-j 4`; quem não mexe nos headers do código gerado usa
  `-DXF_GAME_LIBS=F:/RECOMPs/external/game-libs/<commit>`.
- **Sem Vulkan SDK para compilar** (dono, 2026-11-08, modelo do Xenia): `cmake/vulkan.cmake` baixa Vulkan-Headers,
  SPIRV-Headers e volk; o loader do Vulkan é carregado em runtime (`vk::load_vulkan`, volk); os shaders internos
  ficam compilados e commitados em `src/shaders/spirv/*.inc`. Com o glslc presente o build os recompila quando a
  fonte muda (`XF_SHADERS_REGENERATE`): **commitar o `.inc` junto com o shader**.
- C++20, CMake + Ninja + clang; namespace `xf`; estilo do `.clang-format` (4 espaços, 120 colunas).
  Build: `cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`,
  `cmake --build build`, `ctest --test-dir build`. Testes em `tests/` com o `CHECK` de `tests/check.h`.
- **Referências MIT da hedge-dev**: `../XenonRecomp` e `../XenosRecomp` (clonados). Pode usar código deles quando
  ajudar, com o aviso MIT no topo do arquivo e um `LICENSE-hedge-dev` na raiz quando algo for copiado. **Nunca** o
  `thirdparty/disasm` do XenonRecomp (binutils, GPL). Eles referenciam o Xenia; o oráculo continua sendo o Xenia.
- **Oráculo = `../xenia-canary`** (BSD), compilado em `build/bin/Windows/Release` com
  `-DXENIA_ENABLE_GPU_TRACE=ON -DXENIA_BUILD_MISC=ON` (preset `vs`; o `build/version.h` foi escrito à mão, como o
  `xb` faria). O `xenia_canary.exe` baixado (release) **não grava traces**. Traces e PNGs de referência ficam em
  `../traces/` (fora dos repositórios): `xenia_canary.exe --trace_gpu_prefix=<pasta> --trace_gpu_frames=N,...`
  (ou F4 jogando), `xenia-gpu-vulkan-trace-dump.exe --target_trace_file=<xtr> --trace_dump_path=<pasta>`.
  `tools/capture_traces.ps1` grava e fecha o Xenia sozinho (testado). A primeira execução do build novo fechou
  sozinha logo após iniciar o jogo (não se repetiu). Código traduzido do
  Xenia leva o aviso BSD no topo do arquivo e é conferido contra o Xenia na mesma sessão. As tabelas de exports
  em `src/exports/` são cópias sem mudança (xenia-canary `838850fed`).
- **Corpus de shaders** (fora do repo, dados de jogo): `../traces/shaders` (13, dos traces via `--dump_shaders`),
  `../traces/ue3_global` (118, do `GlobalShaderCache-Xbox360.bin` do jogo A via `xfeffect`),
  `../external/xna/work/shaders` (10, de `tests/hlsl/`). Rodar: `XF_SHADERS=<pasta> build/shader_compare_test`.
  O `.defs` ao lado de cada shader traz as constantes literais do contêiner (o D3D as carrega com o shader).
- **Renderizador (B3)**: `build/xfreplay <xtr> -o frame.png --compare <png do trace-dump> [--diff d.png] [-v]`;
  regressão: `XF_TRACES=../traces XF_DUMPS=../traces/dumps build/replay_test` (os 31 frames do caminho host e os 33 do exato batem com o Xenia; a referência é o
  dump D3D12 do Xenia, `xenia-gpu-d3d12-trace-dump.exe`, em `../traces/dumps/d3d12`, com o Vulkan onde não há
  D3D12 ou os dois discordam: 2 frames do jogo A). Desde 2026-11-06 o padrão é a EDRAM exata via
  fragment shader interlock (FSI) onde o device tem: aí o `replay_test` compara sozinho com os dumps exatos do
  Xenia em `../traces/dumps/rov` (`rov/d3d12` = ROV, `rov` = FSI do Vulkan); `XF_RENDER_TARGET_PATH=host` volta
  aos render targets do host e aos dumps de antes.
  Saídas do replay em `../traces/replay/`. `XF_VK_VALIDATION=1` liga a validação do Vulkan.
- **Traces das nossas execuções** (o oráculo sem jogar no Xenia): `boot_<jogo> <iso> --trace-frames N,M
  --trace-dir <pasta>` (no player, `XF_TRACE_FRAMES`/`XF_TRACE_DIR`) grava `.xtr` no formato do Xenia; o
  `xenia-gpu-vulkan-trace-dump.exe` desenha a referência. `--press botão@#swap` leva a qualquer cena.
- **Som (A6)**: XMA pelo `xmaframes` do FFmpeg do Xenia (`ffmpeg-xma.dll`, LGPL). Regressão contra o Xenia:
  `XF_XMA_TRACES=../traces/xma build/xma_replay_test` (gravações com `XF_XMA_RECORD=<arquivo.xmar>`; o oráculo
  `xma_oracle` sai de `tools/xenia_oracle/build.bat xma`). `XF_AUDIO_WAV=<prefixo>` grava a mixagem para ouvir.
- **Jogos com código gerado**: `-DXF_GAMES="<nome>;<nome>"` (nomes das pastas em `gen-modules`) gera `boot_<jogo>` e `player_<jogo>` (janela SDL3,
  controle, som), com o código de `../external/gen-modules` (padrão do `XF_GEN_DIR`; `xfrecomp emit` grava lá).
- **Biblioteca do jogo (D1/D2)**: `build/xfbuild <iso> [cache]` recompila o disco numa DLL em cache
  (`<cache>/<digest>-<versão da interface>.dll`; padrão `%LOCALAPPDATA%\xenoforge\cache`; clang e lld, 4 jobs); o
  `boot` e o `player` genéricos a carregam (`--cache <pasta>`, `--game-library <dll>`), os `boot_<jogo>` seguem
  estáticos. Interface: `game_library.h` e `ppc_runtime.h` (código gerado freestanding, sem CRT; docs/runtime.md,
  "The game library"). Mudar os headers do código gerado ou o recompilador muda a versão e invalida o cache.
- **Recompilador (A2)**: desenho em `docs/recompiler.md`. Tabelas de `src/ppc/ppc-instructions.xml` (do Xenia,
  sem mudança) via `python tools/ppc_gen.py` (gera `include/xenoforge/ppc_opcodes.inc`, `src/ppc/decode.inc`,
  `src/ppc/disasm.inc`; commitados). Oráculo: `tools/xenia_oracle/build.bat` compila
  `../external/oracle/ppc_oracle.exe` contra as libs do xenia-canary; `build/xfppc <arquivo> <base>` imprime no
  mesmo formato (comparar com `cmp`). Testes de instrução do Xenia montados por
  `bash tools/assemble_ppc_tests.sh ../rexglue-sdk/tools/binutils ../xenia-canary/src/xenia/cpu/ppc/testing
  ../external/ppc_tests tests/ppc` (566 montam; dcbz128, vpermwi128, vpkd3d128, vspltisw128 não montam com este
  binutils; `tests/ppc` traz os nossos, como lswx/stswx, e o `skip.txt` dos casos do Xenia que o console contradiz).
  Rodar: `XF_PPC_TESTS=../external/ppc_tests build/ppc_test [filtros] [-v]` (interpretador) e, configurando com
  `-DXF_PPC_TESTS_DIR=F:/RECOMPs/external/ppc_tests`, `build/ppc_recompiled_test` (os testes recompilados pelo
  `xfrecomp tests`; ~1 min de build). Descoberta: `build/xfrecomp analyze <iso> --compare <*_init.cpp do rexglue>`.
- O fork do rexglue (`../rexglue-sdk`) fica como está: continua rodando os ports dos jogos A e B até o XenoForge rodá-los.
  Não mexer nele por causa do XenoForge.
- **Nenhum nome de jogo no repositório** (dono, 2026-11-08): nem em código, comentários, testes, CMake, scripts,
  exemplos, READMEs, docs ou mensagens de commit. Escrever "um jogo", "<jogo>", endereços inventados nos
  exemplos; medições por jogo usam "jogo A".."jogo H", com a chave (nomes, title IDs, caminhos dos discos) só
  no `docs/plan.md` local, seção "Test discs". Testar com discos reais à mão pode.
- Nada derivado dos jogos entra aqui (ISOs, XEX, imagens, traces de GPU, dumps). Listas de nomes de funções do
  sistema e versões do XDK são fatos e podem ir para os docs.
- O renderizador é geral (implementa o hardware, nunca casos de jogo); o que um jogo pede e não existe é
  contado. Particularidades de jogo vão no port do jogo, não aqui.
- **Linux e Android (ARM64) ficam encaminhados** (dono, 2026-11-06; `docs/plan.md`, "Hosts"): nada de Win32 fora
  de `_WIN32` com o par POSIX, tamanhos vindos do host, processos via SDL3. Vulkan 1.3 é o piso; acima disso, tudo
  opcional com fallback no caminho do Xenia. A GPU vai além do Xenia onde o AoT ou o Vulkan moderno permitem,
  com o trace do Xenia como imagem a bater ("Beyond Xenia on the GPU").
- Verificação real feita: imagem do `default.xex` do jogo A via `xfinspect --dump-image` idêntica byte a byte a
  imagem tirada à parte pelo script Python do port (PyCryptodome).
