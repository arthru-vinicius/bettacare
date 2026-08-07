/**
 * Tela de abertura.
 *
 * É aqui que a arte fotorrealista aparece — e **só** aqui. O ícone do
 * lançador é a ilustração vetorial, que é outra coisa: ver
 * `scripts/gerar-icones.py`.
 *
 * Não é um componente cliente e não depende de hidratação: com
 * `output: 'export'` o estado inicial do app é `loading`, então esta marcação
 * já vai dentro do `index.html` e pinta no primeiro quadro, antes de o React
 * assumir. É o que faz a abertura parecer instantânea em vez de mostrar um
 * retângulo vazio enquanto o bundle carrega.
 */
export function Carregando() {
  return (
    <div className="splash" role="status" aria-live="polite">
      {/* eslint-disable-next-line @next/next/no-img-element */}
      <img
        className="splash-arte"
        src="/icons/splash-512.png"
        alt=""
        width={512}
        height={512}
        // `eager` + alta prioridade: é a única imagem que importa neste
        // momento, e adiar seria adiar a própria tela.
        loading="eager"
        fetchPriority="high"
      />
      <span className="splash-nome">BettaCare</span>
      <span className="splash-dica">Conectando ao aquário…</span>
    </div>
  );
}
