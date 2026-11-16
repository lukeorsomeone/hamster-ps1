import sys, glob, os
from PIL import Image, ImageDraw, ImageFont
font = ImageFont.load_default()
for ppm in sorted(glob.glob('out/*.ppm')):
    im = Image.open(ppm).convert('RGB')
    d = ImageDraw.Draw(im)

    im = im.resize((960, 720), Image.NEAREST)
    im.save(ppm[:-4] + '.png')
imgs = [Image.open(p) for p in sorted(glob.glob('out/*.png'))]
if imgs:
    cols = 3; rows = (len(imgs) + 2) // 3
    sheet = Image.new('RGB', (cols * 480, rows * 360))
    for i, im in enumerate(imgs):
        sheet.paste(im.resize((480, 360)), ((i % cols) * 480, (i // cols) * 360))
    sheet.save('out/sheet.png')
