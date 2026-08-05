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
    icons: [
      { src: "/icons/icon-192.png", sizes: "192x192", type: "image/png" },
      { src: "/icons/icon-512.png", sizes: "512x512", type: "image/png" },
      {
        src: "/icons/icon-512.png",
        sizes: "512x512",
        type: "image/png",
        purpose: "maskable",
      },
    ],
  };
}
