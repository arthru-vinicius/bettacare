import type { Metadata, Viewport } from "next";

import "./globals.css";

export const metadata: Metadata = {
  title: "BettaCare",
  description: "Controle e monitoramento do aquário",
  appleWebApp: {
    capable: true,
    title: "BettaCare",
    statusBarStyle: "black-translucent",
  },
  icons: {
    icon: "/icons/icon-192.png",
    apple: "/icons/apple-touch-icon.png",
  },
};

export const viewport: Viewport = {
  themeColor: "#070d1a",
  width: "device-width",
  initialScale: 1,
  maximumScale: 1,
  userScalable: false,
  // A barra de status do iOS invade a área do app quando instalado; sem isto o
  // cabeçalho fica embaixo do relógio do sistema.
  viewportFit: "cover",
};

export default function RootLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <html lang="pt-BR">
      <body>{children}</body>
    </html>
  );
}
