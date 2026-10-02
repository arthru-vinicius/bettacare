#!/bin/sh
# Testes de host do módulo alimentador: cada test_*.cpp inclui o .cpp real de
# firmware/feeder-module (a lógica pura, sem hardware) e roda no PC.
#
# Uso: sh firmware/feeder-module/test/run.sh        (de qualquer diretório)
set -u
cd "$(dirname "$0")"
mkdir -p build

falhou=0
for teste in test_*.cpp; do
  nome="${teste%.cpp}"
  echo "== $nome"
  if ! g++ -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
         -I .. "$teste" -o "build/$nome"; then
    echo "   NAO COMPILOU"
    falhou=1
    continue
  fi
  "./build/$nome" || falhou=1
done

if [ "$falhou" -ne 0 ]; then
  echo
  echo "ALGUM TESTE FALHOU"
fi
exit "$falhou"
