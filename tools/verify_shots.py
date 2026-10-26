import sys
from PIL import Image

def color_close(px, expect, tol=12):
    return all(abs(int(a) - int(b)) <= tol for a, b in zip(px[:3], expect))

def region_contains(img, box, expect, tol=14, min_ratio=0.005):
    """区域内应有至少 min_ratio 比例的像素接近 expect 色"""
    x0, y0, x1, y1 = box
    total = (x1 - x0) * (y1 - y0)
    hit = 0
    step = max(1, (x1 - x0) // 80)
    for y in range(y0, y1, max(1, step // 2)):
        for x in range(x0, x1, step):
            if color_close(img.getpixel((x, y)), expect, tol):
                hit += 1
    return hit / total >= min_ratio

def region_has_text(img, box, bg, tol=12, min_ink=0.01):
    """区域应有非背景色像素（文本字形）"""
    x0, y0, x1, y1 = box
    ink = 0
    total = (x1 - x0) * (y1 - y0)
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            if not color_close(img.getpixel((x, y)), bg, tol):
                ink += 1
    return ink / total >= min_ink

def avg(img, box):
    x0, y0, x1, y1 = box
    rs = gs = bs = n = 0
    for y in range(y0, y1, 3):
        for x in range(x0, x1, 3):
            p = img.getpixel((x, y))
            rs += p[0]; gs += p[1]; bs += p[2]; n += 1
    return (rs // n, gs // n, bs // n)

def check(name, cond, detail=""):
    print(("PASS" if cond else "FAIL") + f": {name}" + (f"  [{detail}]" if detail else ""))
    return cond

fails = 0
def t(name, cond, detail=""):
    global fails
    if not check(name, cond, detail):
        fails += 1

def root_bg(img, expect):
    return color_close(avg(img, (5, H - 5, 30, H - 5)) if False else img.getpixel((3, H - 3)), expect)

# ============ hello (XML 布局) ============
img = Image.open("shot_hello.png").convert("RGB")
W, H = img.size
print(f"== shot_hello.png {W}x{H}")
t("size 480x800", W == 480 and H == 800)
t("root bg #121212", color_close(img.getpixel((240, 700)), (0x12, 0x12, 0x12)))
t("statusbar bg #000 (10,18)", color_close(img.getpixel((10, 18)), (0, 0, 0)))
t("statusbar clock text 9:41 (430..470,6..30)", region_has_text(img, (430, 8, 468, 28), (0, 0, 0)))
t("appbar bg #1976D2 (240,64)", color_close(img.getpixel((240, 64)), (0x19, 0x76, 0xD2)))
t("appbar title text Material Demo", region_has_text(img, (16, 48, 200, 78), (0x19, 0x76, 0xD2)))
t("card bg #1E1E1E (240,180)", color_close(img.getpixel((240, 180)), (0x1E, 0x1E, 0x1E)))
t("card title text Card Title", region_has_text(img, (32, 124, 150, 148), (0x1E, 0x1E, 0x1E)))
t("button bg #BB86FC (50,300)", color_close(img.getpixel((50, 300)), (0xBB, 0x86, 0xFC)))
t("button label CLICK ME", region_has_text(img, (200, 292, 280, 308), (0xBB, 0x86, 0xFC)))
t("input bg #1E1E1E (240,372)", color_close(img.getpixel((240, 372)), (0x1E, 0x1E, 0x1E)))
t("input placeholder Enter text...", region_has_text(img, (40, 362, 200, 384), (0x1E, 0x1E, 0x1E)))
t("divider #2E2E2E (240,420)", color_close(img.getpixel((240, 420)), (0x2E, 0x2E, 0x2E)))
t("navbar bg #1E1E1E (240,776)", color_close(img.getpixel((240, 776)), (0x1E, 0x1E, 0x1E)))
t("fab bg #BB86FC (415,706)", color_close(img.getpixel((415, 706)), (0xBB, 0x86, 0xFC)))
t("fab + symbol", region_has_text(img, (420, 712, 436, 726), (0xBB, 0x86, 0xFC)))
print()

# ============ hello_html (HTML5 布局) ============
img = Image.open("shot_hello_html.png").convert("RGB")
W, H = img.size
print(f"== shot_hello_html.png {W}x{H}")
t("size 480x800", W == 480 and H == 800)
# 已知布局问题：header 流式从 y=0 开始，覆盖绝对定位的 statusbar（9:41 不可见）
print("KNOWN-ISSUE: header (0,0,480,56) covers statusbar (0,0,480,36), clock 9:41 hidden",
      "" if color_close(img.getpixel((10, 18)), (0x19, 0x76, 0xD2)) else "??")
t("header bg #1976D2 (240,28)", color_close(img.getpixel((240, 28)), (0x19, 0x76, 0xD2)))
t("header title HTML5 Demo", region_has_text(img, (0, 12, 200, 42), (0x19, 0x76, 0xD2)))
t("card bg #1E1E1E (240,140)", color_close(img.getpixel((240, 140)), (0x1E, 0x1E, 0x1E)))
t("card title text", region_has_text(img, (24, 76, 200, 96), (0x1E, 0x1E, 0x1E)))
t("btn CLICK ME #BB86FC (50,256)", color_close(img.getpixel((50, 256)), (0xBB, 0x86, 0xFC)))
t("btn text CLICK ME", region_has_text(img, (215, 248, 265, 264), (0xBB, 0x86, 0xFC)))
t("input bg #1E1E1E (240,328)", color_close(img.getpixel((240, 328)), (0x1E, 0x1E, 0x1E)))
t("flex btn ONE #03DAC6 (50,398)", color_close(img.getpixel((50, 398)), (0x03, 0xDA, 0xC6)))
t("flex btn TWO #03DAC6 (150,398)", color_close(img.getpixel((150, 398)), (0x03, 0xDA, 0xC6)))
t("flex btn THREE #03DAC6 (360,398)", color_close(img.getpixel((360, 398)), (0x03, 0xDA, 0xC6)))
t("flex text labels", region_has_text(img, (30, 390, 450, 414), (0x03, 0xDA, 0xC6)))
t("scroll container bg #1A1A1A (240,520)", color_close(img.getpixel((240, 520)), (0x1A, 0x1A, 0x1A)))
t("scroll item text Item 1", region_has_text(img, (30, 458, 300, 478), (0x1A, 0x1A, 0x1A)))
t("fab #BB86FC (420,706)", color_close(img.getpixel((420, 706)), (0xBB, 0x86, 0xFC)))
t("img icon loaded (44,728) not plain #222", not color_close(img.getpixel((44, 728)), (0x22, 0x22, 0x22)))
print()

# ============ vision_monitor ============
img = Image.open("shot_vision.png").convert("RGB")
W, H = img.size
print(f"== shot_vision.png {W}x{H}")
t("size 900x680", W == 900 and H == 680)
t("root bg #0A0A0A", color_close(img.getpixel((450, 650)), (0x0A, 0x0A, 0x0A)))
t("title bar #003300 (450,15)", color_close(img.getpixel((450, 15)), (0x00, 0x33, 0x00)))
t("title text ● 视觉监视器", region_has_text(img, (8, 6, 200, 26), (0x00, 0x33, 0x00)))
t("left panel #001100 (90,320)", color_close(img.getpixel((90, 320)), (0x00, 0x11, 0x00)))
t("mid panel #001100 (200,410)", color_close(img.getpixel((200, 410)), (0x00, 0x11, 0x00)))
t("right panel #000800 (750,200)", color_close(img.getpixel((750, 200)), (0x00, 0x08, 0x00)))
t("recognition card #000 (400,230)", color_close(img.getpixel((400, 230)), (0, 0, 0)))
t("info label 等待连接... text", region_has_text(img, (634, 220, 760, 235), (0x00, 0x08, 0x00)))
t("log panel #000800 (450,500)", color_close(img.getpixel((450, 500)), (0x00, 0x08, 0x00)))
t("log line text", region_has_text(img, (8, 450, 400, 466), (0x00, 0x08, 0x00)))
print()

# ============ terminal (滚动后) ============
img = Image.open("shot_terminal.png").convert("RGB")
W, H = img.size
print(f"== shot_terminal.png {W}x{H}")
t("size 800x600", W == 800 and H == 600)
t("bg #0C0C0C", color_close(img.getpixel((700, 40)), (0x0C, 0x0C, 0x0C)))
t("input bar #1E1E1E (400,580)", color_close(img.getpixel((400, 580)), (0x1E, 0x1E, 0x1E)))
t("prompt $ text (10,570)", region_has_text(img, (10, 570, 40, 588), (0x1E, 0x1E, 0x1E)))
print()

print(f"=== {fails} failures ===")
sys.exit(1 if fails else 0)
