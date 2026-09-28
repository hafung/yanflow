"""Build Windows icon resources from the checked-in YanFlow artwork.

Requires Pillow. Run from any directory with ``py build/windows/package-icons.py``.
"""

from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[2]
ICONS = ROOT / "assets" / "icons"
SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]


def render(source: str, output: str) -> None:
    with Image.open(ICONS / source) as artwork:
        artwork = artwork.convert("RGBA")
        side = min(artwork.size) - 20
        left = (artwork.width - side) // 2
        top = (artwork.height - side) // 2
        artwork = artwork.crop((left, top, left + side, top + side))
        artwork = artwork.resize((1024, 1024), Image.Resampling.LANCZOS)

    # The generated source has a white presentation canvas around the icon.
    # Clip it to the intended rounded-square silhouette before packaging.
    mask = Image.new("L", artwork.size, 0)
    draw = ImageDraw.Draw(mask)
    draw.rounded_rectangle((0, 0, 1023, 1023), radius=245, fill=255)
    artwork.putalpha(mask)

    png = artwork.resize((256, 256), Image.Resampling.LANCZOS)
    png.save(ICONS / f"{output}.png", optimize=True)
    artwork.save(ICONS / f"{output}.ico", format="ICO", sizes=SIZES)


render("yanflow-source-idle.png", "yanflow-app")
render("yanflow-source-idle.png", "yanflow-floating")
render("yanflow-source-listening.png", "yanflow-listening")
