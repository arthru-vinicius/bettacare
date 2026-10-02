/*
 * Service worker do BettaCare.
 *
 * Escrito à mão, ~120 linhas, em vez de Serwist ou next-pwa. O app tem quatro
 * telas e o requisito é controle exato do momento do `skipWaiting` — que é
 * justamente a parte que as bibliotecas abstraem. A dependência custaria mais
 * em integração com `output: 'export'` do que economiza em código.
 *
 * O `__BUILD_ID__` é substituído pelo `scripts/stamp-version.mjs` depois do
 * build. É ele que nomeia o cache, e é por isso que uma build nova invalida
 * tudo automaticamente.
 */

const BUILD_ID = "__BUILD_ID__";
const CACHE = `bettacare-${BUILD_ID}`;

/**
 * O casco do app. Não inclui `/api/*` nem `/version.json`: o primeiro é estado
 * corrente e o segundo é justamente o que detecta uma versão nova — servir
 * qualquer um dos dois do cache anularia o mecanismo.
 */
const SHELL = [
  "/",
  "/manifest.webmanifest",
  // A arte da tela de abertura entra no casco: ela é a primeira coisa que o
  // usuário vê, e buscá-la da rede na abertura anularia o ganho de ter cache.
  "/icons/splash-512.png",
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches
      .open(CACHE)
      .then((cache) => cache.addAll(SHELL))
      // Não chama skipWaiting aqui de propósito: o worker novo fica em espera
      // até o usuário aceitar (ou até a atualização ser forçada). Trocar por
      // baixo dos pés de quem está no meio de uma ação é como se perde estado.
      .catch(() => undefined),
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    (async () => {
      const nomes = await caches.keys();
      await Promise.all(
        nomes
          .filter((n) => n.startsWith("bettacare-") && n !== CACHE)
          .map((n) => caches.delete(n)),
      );
      await self.clients.claim();
    })(),
  );
});

self.addEventListener("message", (event) => {
  if (event.data === "SKIP_WAITING") self.skipWaiting();
});

self.addEventListener("fetch", (event) => {
  const req = event.request;
  if (req.method !== "GET") return;

  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;

  // Nunca do cache: estado corrente e o carimbo de versão.
  if (url.pathname.startsWith("/api/") || url.pathname === "/version.json") {
    return;
  }

  /*
   * Assets com hash no nome (`/_next/static/**`) nunca mudam de conteúdo:
   * cache primeiro, sem nem consultar a rede. É o que faz o app abrir
   * instantaneamente na segunda visita.
   */
  if (url.pathname.startsWith("/_next/static/")) {
    event.respondWith(
      caches.match(req).then(
        (hit) =>
          hit ??
          fetch(req).then((resp) => {
            if (resp.ok) {
              const copia = resp.clone();
              void caches.open(CACHE).then((c) => c.put(req, copia));
            }
            return resp;
          }),
      ),
    );
    return;
  }

  /*
   * Navegação: rede primeiro, cache como rede de segurança.
   *
   * A ordem importa. Cache primeiro deixaria o usuário numa build velha até o
   * worker trocar; rede primeiro garante o HTML novo assim que houver rede, e
   * o cache só entra quando não há — que é quando ele salva o dia.
   */
  if (req.mode === "navigate") {
    event.respondWith(
      fetch(req)
        .then((resp) => {
          if (resp.ok) {
            const copia = resp.clone();
            void caches.open(CACHE).then((c) => c.put("/", copia));
          }
          return resp;
        })
        .catch(async () => {
          const hit = await caches.match("/");
          return (
            hit ??
            new Response("Sem conexão e sem cópia local.", {
              status: 503,
              headers: { "content-type": "text/plain; charset=utf-8" },
            })
          );
        }),
    );
    return;
  }

  // Demais recursos (ícones, manifest): cache com revalidação em segundo plano.
  event.respondWith(
    caches.match(req).then((hit) => {
      const rede = fetch(req)
        .then((resp) => {
          if (resp.ok) {
            const copia = resp.clone();
            void caches.open(CACHE).then((c) => c.put(req, copia));
          }
          return resp;
        })
        .catch(() => hit);
      return hit ?? rede;
    }),
  );
});

/*
 * Notificações push.
 *
 * O alerta chega pelo push service do próprio navegador (FCM no Android, WNS
 * no Windows) e é entregue **mesmo com o PWA fechado** — é o que diferencia um
 * alerta de verdade de um aviso que só aparece quando alguém já está olhando.
 */
self.addEventListener("push", (event) => {
  let dados = { title: "BettaCare", body: "Há um alerta no aquário.", tag: "geral" };
  try {
    if (event.data) dados = { ...dados, ...event.data.json() };
  } catch {
    // Payload ilegível não pode engolir a notificação: melhor um aviso
    // genérico do que silêncio, porque o silêncio é indistinguível de "está
    // tudo bem".
  }

  event.waitUntil(
    self.registration.showNotification(dados.title, {
      body: dados.body,
      // `tag` agrupa: um alerta novo do mesmo componente substitui o anterior
      // em vez de empilhar dez avisos da mesma ventoinha.
      tag: dados.tag,
      icon: "/icons/icon-192.png",
      badge: "/icons/icon-192.png",
      requireInteraction: dados.requireInteraction === true,
      renotify: true,
    }),
  );
});

/*
 * Tocar na notificação abre o app — reaproveitando a aba já aberta, se houver.
 * Abrir uma segunda janela do mesmo PWA seria confuso e perderia o estado.
 */
self.addEventListener("notificationclick", (event) => {
  event.notification.close();

  event.waitUntil(
    (async () => {
      const abertas = await self.clients.matchAll({
        type: "window",
        includeUncontrolled: true,
      });

      for (const cliente of abertas) {
        if (new URL(cliente.url).origin === self.location.origin) {
          await cliente.focus();
          return;
        }
      }

      await self.clients.openWindow("/");
    })(),
  );
});
