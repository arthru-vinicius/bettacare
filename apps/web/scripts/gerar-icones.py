#!/usr/bin/env python3
"""
Gera os ícones do PWA a partir dos masters em `apps/web/assets/`.

Rodado à mão, quando a arte muda — não faz parte do build:

    python apps/web/scripts/gerar-icones.py

Existem **duas artes com papéis diferentes**, e trocá-las foi um bug real:

  - `betta-lancador-*`   ilustração vetorial, contorno limpo, fundo transparente.
                         Vai para a gaveta de apps e a tela inicial, onde é
                         desenhada a 48 px e precisa continuar legível.
  - `betta-carregando-*` render fotorrealista com brilho escuro, sangrando até a
                         borda. Só aparece na tela de carregamento, grande.

A arte de carregamento vira mingau a 48 px e a do lançador fica sem graça em
tela cheia. Cada uma no seu lugar.

Este arquivo existe para registrar a **zona segura do maskable**: sem ela o
Android corta as caudas do peixe, e isso é invisível no PNG pronto.
"""

from pathlib import Path

from PIL import Image

RAIZ = Path(__file__).resolve().parents[1]
ASSETS = RAIZ / "assets"
SAIDA = RAIZ / "public" / "icons"

# Mesmo `background_color` do manifest. Onde é preciso opacidade, o fundo é
# este — assim o ícone não muda de cor conforme o tema do sistema.
FUNDO = (7, 13, 26, 255)

# Android recorta o ícone maskable num formato que o fabricante escolhe:
# círculo, squircle, gota. Só um círculo central de 80% do lado é garantido.
# 76% dá uma folga sobre o mínimo, porque o peixe é largo e as caudas são
# justamente o que se perderia.
FRACAO_ZONA_SEGURA = 0.76

# O iOS aplica cantos arredondados e não recorta agressivamente, então cabe
# mais arte — mas ainda não pode encostar na borda.
FRACAO_APPLE = 0.88


def encaixar(master: Image.Image, lado: int, fracao: float, fundo=None) -> Image.Image:
    """Encaixa o conteúdo visível do master numa fração do quadro, centralizado."""
    # A caixa do conteúdo, não a do arquivo: os masters têm margens
    # transparentes diferentes entre si, e ignorá-las faria cada ícone sair
    # com um tamanho aparente distinto.
    caixa = master.getchannel("A").point(lambda p: 255 if p > 8 else 0).getbbox()
    conteudo = master.crop(caixa)

    alvo = int(lado * fracao)
    escala = min(alvo / conteudo.width, alvo / conteudo.height)
    novo = (max(1, round(conteudo.width * escala)), max(1, round(conteudo.height * escala)))
    conteudo = conteudo.resize(novo, Image.LANCZOS)

    quadro = Image.new("RGBA", (lado, lado), fundo if fundo else (0, 0, 0, 0))
    quadro.alpha_composite(
        conteudo, ((lado - novo[0]) // 2, (lado - novo[1]) // 2)
    )
    return quadro


def salvar(im: Image.Image, nome: str) -> None:
    destino = SAIDA / nome
    im.save(destino, "PNG", optimize=True)
    kb = destino.stat().st_size / 1024
    print(f"  {nome:26} {im.size[0]}x{im.size[1]}  {kb:6.1f} KB")


def main() -> None:
    SAIDA.mkdir(parents=True, exist_ok=True)
    lancador = Image.open(ASSETS / "betta-lancador-1024.png").convert("RGBA")
    carregando = Image.open(ASSETS / "betta-carregando-1024.png").convert("RGBA")

    print("lançador (gaveta de apps e tela inicial):")
    # Transparente e sangrando: é o que o Android usa quando NÃO aplica máscara,
    # e o que fica bom sobre qualquer papel de parede.
    salvar(encaixar(lancador, 192, 1.0), "icon-192.png")
    salvar(encaixar(lancador, 512, 1.0), "icon-512.png")
    salvar(
        encaixar(lancador, 512, FRACAO_ZONA_SEGURA, FUNDO), "icon-maskable-512.png"
    )
    # 180 é o tamanho que o iOS pede. Opaco porque o iOS pinta transparência de
    # preto, e um fundo explícito é previsível.
    salvar(encaixar(lancador, 180, FRACAO_APPLE, FUNDO), "apple-touch-icon.png")

    print("carregamento (só na abertura do app):")
    salvar(encaixar(carregando, 512, 1.0), "splash-512.png")


if __name__ == "__main__":
    main()
