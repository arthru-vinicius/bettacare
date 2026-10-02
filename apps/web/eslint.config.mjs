import { defineConfig, globalIgnores } from "eslint/config";
import nextVitals from "eslint-config-next/core-web-vitals";
import nextTs from "eslint-config-next/typescript";

/**
 * O `next lint` saiu no Next 16; o lint agora é o ESLint direto, com a
 * configuração recomendada do Next (Core Web Vitals + TypeScript).
 */
const eslintConfig = defineConfig([
  ...nextVitals,
  ...nextTs,
  {
    rules: {
      /*
       * Aviso, não erro. A regra (nova, da linha do React Compiler) reprova
       * `setState` síncrono num efeito, e o app faz isso de propósito em dez
       * lugares: ligar o "carregando" no início de uma busca e ressincronizar
       * um formulário com o dado do servidor quando não há edição local. Não é
       * bug — é uma renderização a mais. Refatorar para o padrão sem efeito
       * mexe em telas em uso e só se valida no navegador; fica visível aqui
       * até isso ser feito com calma.
       */
      "react-hooks/set-state-in-effect": "warn",
    },
  },
  globalIgnores([
    // Os padrões do eslint-config-next:
    ".next/**",
    "out/**",
    "build/**",
    "next-env.d.ts",
    // Gerados no build: o service worker carimbado e a versão.
    "public/sw.js",
  ]),
]);

export default eslintConfig;
