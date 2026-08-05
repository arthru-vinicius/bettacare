import { createHash, timingSafeEqual } from "node:crypto";

/**
 * Comparação do token de ingestão em **tempo constante**, como o contrato do
 * homelab exige — uma comparação com curto-circuito vaza o prefixo correto
 * pela diferença no tempo de resposta.
 *
 * Os dois lados passam por SHA-256 antes da comparação. Isso resolve o
 * problema prático do `timingSafeEqual`, que lança exceção quando os buffers
 * têm tamanhos diferentes: o digest tem sempre 32 bytes, então o comprimento
 * do token recebido também deixa de vazar.
 */
export function tokenMatches(
  received: string | undefined,
  expected: string,
): boolean {
  if (typeof received !== "string" || received.length === 0) return false;

  const a = createHash("sha256").update(received, "utf8").digest();
  const b = createHash("sha256").update(expected, "utf8").digest();

  return timingSafeEqual(a, b);
}
