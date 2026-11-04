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
- **Oráculo = `../xenia-canary`** (BSD; ainda não compilado, submódulos não baixados). Código traduzido do
  Xenia leva o aviso BSD no topo do arquivo e é conferido contra o Xenia na mesma sessão. As tabelas de exports
  em `src/exports/` são cópias sem mudança (xenia-canary `838850fed`).
- O fork do rexglue (`../rexglue-sdk`) fica como está: continua rodando Chaves e Ben 10 até o XenoForge rodá-los.
  Não mexer nele por causa do XenoForge.
- Nada derivado dos jogos entra aqui (ISOs, XEX, imagens, traces de GPU, dumps). Listas de nomes de funções do
  sistema e versões do XDK são fatos e podem ir para os docs.
- O renderizador é geral (implementa o hardware, nunca casos de jogo); o que um jogo pede e não existe é
  contado. Particularidades de jogo vão no port do jogo, não aqui.
- Verificação real feita: imagem do `default.xex` do Chaves via `xfinspect --dump-image` idêntica byte a byte a
  `../Chaves/re/image.bin` (script Python com PyCryptodome).
