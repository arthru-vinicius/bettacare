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
     * Só a arte do lançador entra aqui — é daqui que saem o ícone da gaveta de
     * apps e o da tela inicial.
     *
     * O `maskable` é um arquivo próprio, não o mesmo 512 reaproveitado. O
     * Android recorta o maskable no formato que o fabricante escolher, e a
     * ilustração sangra até a borda: reusar o `any` como maskable cortava as
     * caudas do peixe. `icon-maskable-512.png` já vem com a zona segura —
     * ver `scripts/gerar-icones.py`.
     */
    icons: [
      { src: "/icons/icon-192.png", sizes: "192x192", type: "image/png", purpose: "any" },
      { src: "/icons/icon-512.png", sizes: "512x512", type: "image/png", purpose: "any" },
      {
        src: "/icons/icon-maskable-512.png",
        sizes: "512x512",
        type: "image/png",
        purpose: "maskable",
      },
    ],
  };
}
