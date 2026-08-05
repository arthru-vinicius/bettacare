import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  /**
   * Export estático. A interface é servida pelo Hono, no mesmo processo do
   * servidor — não há Next.js server, não há proxy interno, não há segundo
   * container.
   */
  output: "export",

  /**
   * Sem otimização de imagem: ela exige um servidor Next em runtime, que não
   * existe aqui. São dois ícones de PWA, então não há o que otimizar.
   */
  images: { unoptimized: true },

  /**
   * O contrato é publicado como TypeScript/ESM no workspace. Sem isto o Next
   * não o transpila e o import quebra no build.
   */
  transpilePackages: ["@bettacare/contract"],

  /**
   * Os cabeçalhos de cache **não** ficam aqui: `headers()` não tem efeito com
   * `output: 'export'`, porque não há servidor Next para aplicá-los. Quem os
   * define é o Hono, em `apps/server/src/static.ts`. É lá que a briga da
   * atualização do PWA é ganha.
   */
};

export default nextConfig;
