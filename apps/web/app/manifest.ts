import type { MetadataRoute } from "next";

/**
 * Com `output: 'export'` o Next exige que a rota do manifest declare que é
 * estática — não há servidor em runtime para gerá-la sob demanda.
 */
export const dynamic = "force-static";

export default function manifest(): MetadataRoute.Manifest {
  return {
    name: "BettaCare",
    short_name: "BettaCare",
    description: "Controle e monitoramento do aquário",
    // Relativo: a interface fala com a própria origem, e o mesmo artefato roda
    // em qualquer host sem nada congelado no bundle.
    start_url: "/",
    scope: "/",
    display: "standalone",
    orientation: "portrait",
    background_color: "#070d1a",
    theme_color: "#070d1a",
    lang: "pt-BR",
    /**
     * Daqui saem o ícone da gaveta de apps **e** a tela de abertura do Android:
     * o manifest não tem campo de splash, e o Chrome monta a abertura com
     * `background_color` e um destes ícones, preferindo os `maskable`.
     *
     * Os dois maskable se separam pelo tamanho, e os tamanhos não são
     * arbitrários: a gaveta escolhe o menor que alcance 83dp, a abertura o
     * menor que alcance 128dp (vezes a densidade da tela). Com o lançador em
     * 332 e a arte de carregamento em 512, a gaveta fica com o lançador e a
     * abertura com a arte de carregamento — a conta e os limites estão em
     * `scripts/gerar-icones.py`. Não acrescente outro maskable sem refazê-la.
     *
     * Os maskable são arquivos próprios, com zona segura: o Android recorta no
     * formato que o fabricante escolher, e reusar o `any` cortava as caudas.
     */
    icons: [
      { src: "/icons/icon-192.png", sizes: "192x192", type: "image/png", purpose: "any" },
      { src: "/icons/icon-512.png", sizes: "512x512", type: "image/png", purpose: "any" },
      {
        src: "/icons/icon-maskable-332.png",
        sizes: "332x332",
        type: "image/png",
        purpose: "maskable",
      },
      {
        src: "/icons/splash-maskable-512.png",
        sizes: "512x512",
        type: "image/png",
        purpose: "maskable",
      },
    ],
  };
}
