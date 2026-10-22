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
                         borda. Só aparece grande: na tela de abertura do
                         Android e na tela de carregamento do app.

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

# ── Splash do Android ─────────────────────────────────────────────────────
#
# O manifest não tem campo de splash no Android: o Chrome monta a tela de
# abertura com a cor de fundo e **um dos ícones de `icons`**, preferindo os
# `maskable` — o mesmo grupo de onde sai o ícone da gaveta. Sem distinção, o
# lançador aparecia grande na abertura (2026-10).
#
# O que separa os dois é o tamanho (Chromium, `shortcut_info.cc` e
# `manifest_icon_selector.cc`): cada um pega o **menor** maskable que alcance o
# seu tamanho ideal — 83dp para a gaveta, 128dp para o splash, vezes a
# densidade da tela. Com o lançador em 332 px (83 × 4) e a arte de
# carregamento em 512:
#   - a gaveta escolhe o lançador em qualquer tela até densidade 4,0;
#   - o splash escolhe a arte de carregamento quando 128dp passa de 332 px,
#     isto é, densidade acima de ~2,6 — o padrão de Pixel e Galaxy atuais.
#     Abaixo disso (tela em "zoom pequeno"), o splash volta a mostrar o
#     lançador, como antes; nunca o contrário.
# Não pode haver outro maskable de 512: o empate seria decidido pela ordem.
LADO_MASKABLE_LANCADOR = 332
LADO_SPLASH = 512

# O splash desenha o maskable como ícone adaptativo: só os 72/108 centrais
# (2/3 do lado) aparecem, recortados no formato do aparelho — no círculo, raio
# de 1/3 do quadro. O peixe inteiro cabe num raio de 0,30, com folga.
RAIO_SPLASH = 0.30


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


def encaixar_no_circulo(master: Image.Image, lado: int, raio: float, fundo) -> Image.Image:
    """
    Encaixa o conteúdo visível num círculo central de `raio` × lado.

    Para máscaras circulares a caixa retangular engana: o peixe é redondo, e
    o que importa é o pixel visível mais distante do centro, não o canto da
    caixa (que é transparente).
    """
    alfa = master.getchannel("A").point(lambda p: 255 if p > 24 else 0)
    caixa = alfa.getbbox()
    conteudo = master.crop(caixa)
    mascara = alfa.crop(caixa)

    cx, cy = conteudo.width / 2, conteudo.height / 2
    dados = mascara.load()
    maior = 0.0
    for y in range(0, conteudo.height, 2):
        for x in range(0, conteudo.width, 2):
            if dados[x, y]:
                maior = max(maior, ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5)

    escala = (raio * lado) / maior
    novo = (max(1, round(conteudo.width * escala)), max(1, round(conteudo.height * escala)))
    conteudo = conteudo.resize(novo, Image.LANCZOS)

    quadro = Image.new("RGBA", (lado, lado), fundo)
    quadro.alpha_composite(conteudo, ((lado - novo[0]) // 2, (lado - novo[1]) // 2))
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
        encaixar(lancador, LADO_MASKABLE_LANCADOR, FRACAO_ZONA_SEGURA, FUNDO),
        f"icon-maskable-{LADO_MASKABLE_LANCADOR}.png",
    )
    # 180 é o tamanho que o iOS pede. Opaco porque o iOS pinta transparência de
    # preto, e um fundo explícito é previsível.
    salvar(encaixar(lancador, 180, FRACAO_APPLE, FUNDO), "apple-touch-icon.png")

    print("carregamento (só na abertura do app):")
    # A tela de carregamento do próprio app (`Carregando.tsx`).
    salvar(encaixar(carregando, 512, 1.0), "splash-512.png")
    # O splash do Android, desenhado pelo Chrome antes do app — ver acima.
    salvar(
        encaixar_no_circulo(carregando, LADO_SPLASH, RAIO_SPLASH, FUNDO),
        f"splash-maskable-{LADO_SPLASH}.png",
    )


if __name__ == "__main__":
    main()
