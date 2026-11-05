# XenoForge — instruções para o Claude

Sistema enxuto de Xbox 360 para jogos recompilados estaticamente, escrito do zero com a pesquisa do Xenia
como referência: recompilador, kernel, GPU, som e input só do que os jogos pedem (medido). Mesmo método do
`../runtime` (o do 3DS). Plano: `docs/plan.md`; renderizador: `docs/renderer.md`; o que os jogos importam:
`docs/lean-kernel.md`.

- Repositório local (ainda sem remoto). Commits locais de rotina permitidos; criar repo no GitHub ou dar push
  só quando o dono pedir. Docs, comentários, logs e commits em inglês; só este arquivo em português.
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
  `../traces/ue3_global` (118, do `GlobalShaderCache-Xbox360.bin` do Chaves via `xfeffect`),
  `../external/xna/work/shaders` (10, de `tests/hlsl/`). Rodar: `XF_SHADERS=<pasta> build/shader_compare_test`.
  O `.defs` ao lado de cada shader traz as constantes literais do contêiner (o D3D as carrega com o shader).
- **Renderizador (B3)**: `build/xfreplay <xtr> -o frame.png --compare <png do trace-dump> [--diff d.png] [-v]`;
  regressão: `XF_TRACES=../traces XF_DUMPS=../traces/dumps build/replay_test` (os 11 frames batem com o Xenia).
  Saídas do replay em `../traces/replay/`. `XF_VK_VALIDATION=1` liga a validação do Vulkan.
- **Som (A6)**: XMA pelo `xmaframes` do FFmpeg do Xenia (`ffmpeg-xma.dll`, LGPL). Regressão contra o Xenia:
  `XF_XMA_TRACES=../traces/xma build/xma_replay_test` (gravações com `XF_XMA_RECORD=<arquivo.xmar>`; o oráculo
  `xma_oracle` sai de `tools/xenia_oracle/build.bat xma`). `XF_AUDIO_WAV=<prefixo>` grava a mixagem para ouvir.
- **Jogos com código gerado**: `-DXF_GAMES="chaves;ben10"` gera `boot_<jogo>` e `player_<jogo>` (janela SDL3,
  controle, som), com o código de `../external/gen-modules` (padrão do `XF_GEN_DIR`; `xfrecomp emit` grava lá).
- **Recompilador (A2)**: desenho em `docs/recompiler.md`. Tabelas de `src/ppc/ppc-instructions.xml` (do Xenia,
  sem mudança) via `python tools/ppc_gen.py` (gera `include/xenoforge/ppc_opcodes.inc`, `src/ppc/decode.inc`,
  `src/ppc/disasm.inc`; commitados). Oráculo: `tools/xenia_oracle/build.bat` compila
  `../external/oracle/ppc_oracle.exe` contra as libs do xenia-canary; `build/xfppc <arquivo> <base>` imprime no
  mesmo formato (comparar com `cmp`). Testes de instrução do Xenia montados por
  `bash tools/assemble_ppc_tests.sh ../rexglue-sdk/tools/binutils ../xenia-canary/src/xenia/cpu/ppc/testing
  ../external/ppc_tests` (566 montam; dcbz128, vpermwi128, vpkd3d128, vspltisw128 não montam com este binutils).
  Rodar: `XF_PPC_TESTS=../external/ppc_tests build/ppc_test [filtros] [-v]` (interpretador) e, configurando com
  `-DXF_PPC_TESTS_DIR=F:/RECOMPs/external/ppc_tests`, `build/ppc_recompiled_test` (os testes recompilados pelo
  `xfrecomp tests`; ~1 min de build). Descoberta: `build/xfrecomp analyze <iso> --compare <*_init.cpp do rexglue>`.
- O fork do rexglue (`../rexglue-sdk`) fica como está: continua rodando Chaves e Ben 10 até o XenoForge rodá-los.
  Não mexer nele por causa do XenoForge.
- Nada derivado dos jogos entra aqui (ISOs, XEX, imagens, traces de GPU, dumps). Listas de nomes de funções do
  sistema e versões do XDK são fatos e podem ir para os docs.
- O renderizador é geral (implementa o hardware, nunca casos de jogo); o que um jogo pede e não existe é
  contado. Particularidades de jogo vão no port do jogo, não aqui.
- Verificação real feita: imagem do `default.xex` do Chaves via `xfinspect --dump-image` idêntica byte a byte a
  `../Chaves/re/image.bin` (script Python com PyCryptodome).
