/**
 * Carimba a build depois do `next build`.
 *
 * Faz duas coisas, e as duas são o que torna a atualização do PWA detectável:
 *
 *   1. Escreve `out/version.json` com o identificador desta build.
 *   2. Substitui `__BUILD_ID__` dentro de `out/sw.js`, que é o que nomeia o
 *      cache do service worker — build nova, cache novo, sem intervenção.
 *
 * `BUILD_ID` vem da CI (o SHA do commit). Fora dela, cai no git e depois no
 * relógio, para que um build local também produza um identificador distinto.
 */
import { execSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const OUT = join(process.cwd(), "out");

function resolveBuildId() {
  if (process.env.BUILD_ID) return process.env.BUILD_ID.slice(0, 12);
  try {
    return execSync("git rev-parse --short=10 HEAD", { stdio: ["ignore", "pipe", "ignore"] })
      .toString()
      .trim();
  } catch {
    return `local${Date.now().toString(36)}`;
  }
}

const buildId = resolveBuildId();

const pkg = JSON.parse(readFileSync(join(process.cwd(), "package.json"), "utf8"));

const info = {
  buildId,
  version: pkg.version,
  /**
   * Builds anteriores a este identificador recarregam sem perguntar. Fica nulo
   * no caminho normal: forçar recarga é para quando uma versão quebrada foi ao
   * ar ou o contrato da API mudou de forma incompatível.
   */
  minBuild: process.env.MIN_BUILD ?? null,
  builtAt: new Date().toISOString(),
};

writeFileSync(join(OUT, "version.json"), JSON.stringify(info, null, 2) + "\n");

const swPath = join(OUT, "sw.js");
const sw = readFileSync(swPath, "utf8");
if (!sw.includes("__BUILD_ID__")) {
  // Falha alto: um sw.js sem o carimbo mantém o mesmo nome de cache para
  // sempre, e o app ficaria preso na primeira build instalada — exatamente o
  // problema que este arquivo existe para evitar.
  console.error("ERRO: __BUILD_ID__ não encontrado em out/sw.js");
  process.exit(1);
}
writeFileSync(swPath, sw.replaceAll("__BUILD_ID__", buildId));

console.log(`build ${buildId} carimbada (version.json + sw.js)`);
