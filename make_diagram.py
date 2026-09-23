from PIL import Image, ImageDraw, ImageFont
import os

W, H = 980, 740
img = Image.new("RGB", (W, H), "#f5f5f0")
d = ImageDraw.Draw(img)

def font(sz):
    candidates = [
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "C:/Windows/Fonts/meiryo.ttc",
        "C:/Windows/Fonts/msgothic.ttc",
        "C:/Windows/Fonts/YuGothM.ttc",
    ]
    for p in candidates:
        try: return ImageFont.truetype(p, sz)
        except Exception: pass
    return ImageFont.load_default()

def box(x,y,w,h,label,sub=None,col="#333"):
    d.rectangle([x,y,x+w,y+h], outline=col, width=2 if col=="#000" else 1, fill="white")
    d.text((x+w/2, y+14), text=label, anchor="mm", font=font(14), fill=col)
    if sub: d.text((x+w/2, y+h-16), text=sub, anchor="mm", font=font(10), fill="#777")

def line(x1,y1,x2,y2,col,w=2):
    d.line([x1,y1,x2,y2], fill=col, width=w)

d.text((W/2, 18), text="DIY Nanodrop (UV) — Block Diagram", anchor="mm", font=font(16), fill="#222")
d.text((W/2, 40), text="ESP32-WROOM-32 + 2.8\" ST7789 LCD · 265nm+280nm UVC LED（切替）· SG01S-C18 + TIA + ADS1115", anchor="mm", font=font(11), fill="#555")

box(40,90,150,40,"DC 9V アダプタ",None)
box(380,240,200,160,"ESP32-WROOM-32",None,"#000")
d.text((480,410), text="3.3V / 16MB Flash / PWM(LEDC)", anchor="mm", font=font(10), fill="#555")
d.text((480,262), text="ボード内蔵\n2.8\" ST7789 LCD\n+ タッチ/SD", anchor="mm", font=font(9), fill="#c00")
box(700,150,210,60,"UVC LED 265nm","3535 Vf6-7V ~150mA")
box(700,240,210,60,"UVC LED 280nm","3535 Vf6-7V ~150mA")
box(640,158,55,44,"MOSFET Q1",None)
box(640,248,55,44,"MOSFET Q2",None)
# 電源: LM2596降圧で5V生成 + LED用定電流(LM2596 CC)
box(70,300,180,60,"USB 5V / LM2596\n5V (ESP32ボード)",None)
box(70,400,180,60,"LM2596 定電流\n150mA (LED用)",None)
box(380,520,200,70,"SG01S-C18 + TIA + ADS1115","SiC UV-PD → TIA → 16-bit I2C ADC · 3.3V")
box(120,520,180,70,"2.8\" ST7789 LCD\n(ボード内蔵)",None)

# 9V → LM2596降圧(5V) → MCU
line(140,330,140,380,"#c00",2); d.text((150,360), text="9V", anchor="lm", font=font(9), fill="#c00")
line(140,330,280,330,"#c00",2)
line(250,330,380,330,"#c00",2); d.text((300,320), text="5V", anchor="mm", font=font(9), fill="#c00")
# 9V → LM2596定電流(150mA) → Q1/Q2 → LED
line(140,430,140,470,"#c00",2); d.text((150,450), text="9V", anchor="lm", font=font(9), fill="#c00")
line(140,430,640,430,"#c00",2)
d.text((390,420), text="定電流 150mA", anchor="mm", font=font(9), fill="#c00")
line(640,430,640,202,"#c00",2); d.text((655,320), text="Q1/Q2 (低側スイッチ)", anchor="lm", font=font(8), fill="#c00")
line(695,150,700,180,"#c00",2)
line(695,240,700,270,"#0a0",2)
# ゲート駆動 (MCU→Q1/Q2、5V直結で論理レベル)
line(580,300,640,180,"#c00",2); d.text((605,235), text="PWM Q1", anchor="mm", font=font(9), fill="#c00")
line(580,340,640,270,"#0a0",2); d.text((605,305), text="PWM Q2", anchor="mm", font=font(9), fill="#0a0")
line(480,520,480,400,"#00c",2); d.text((492,470), text="I2C (SDA/SCL)", anchor="lm", font=font(10), fill="#00c")
d.arc((300,470,360,555),start=90,end=180,fill="#666",width=2)
d.text((330,470), text="SDA/SCL", anchor="mm", font=font(10), fill="#666")
line(190,110,380,300,"#c00",2); d.text((255,180), text="VCC", anchor="mm", font=font(10), fill="#c00")

line(60,680,920,680,"#333",3)
d.text((70,702), text="GND（共通グランド: LED・センサー・MCU・ディスプレイ）", anchor="lm", font=font(10), fill="#333")

d.text((40,725), text="Note: ESP32(3.3V) と ADS1115/AS7331(3.3V) は I2C 直結（レベルシフタ不要）。SG01Sの光電流をTIAで電圧化しADS1115でAD変換。吸光度は比I/I0でTIAゲインは不要。校正係数はESP32のEEPROMエミュレーションへ保存(再起動時に自動適用)。", anchor="lm", font=font(8), fill="#888")

# ============================================================
# 光学レイアウト（上から見た図）: V字配置・内傾 LED
#   2つのUVC LED を内側へ約45°傾け、ビームをキュベット中心で交差させる。
#   → 放熱板の干渉なし。単一検出器＋個別校正(K260/K280)で対応。
# ============================================================
d.line([(120,360),(860,360)], fill="#ddd", width=1)

# キュベット（サンプルセル）— 中央下。316Lワッシャー + UV-grade石英窓
cvx, cvy = 490, 560
# アルミ導光パイプ（上からサンプルセルへ）
d.line([cvx-20, 380, cvx-20, cvy-30], fill="#aaa", width=1)
d.line([cvx+20, 380, cvx+20, cvy-30], fill="#aaa", width=1)
# UV-grade石英窓（上）
d.rectangle([cvx-35, cvy-40, cvx+35, cvy-30], outline="#0af", width=2, fill="#e0f4ff")
d.text((cvx, cvy-46), text="UV-grade fused silica 窓", anchor="mm", font=font(8), fill="#0af")
# 316L ワッシャー（光路長0.5mmを定義）
d.rectangle([cvx-35, cvy-30, cvx+35, cvy-29], outline="#444", width=2, fill="#ccc")
d.text((cvx-70, cvy-30), text="316L\nワッシャー\n(内径2.7/厚0.5mm)", anchor="mm", font=font(8), fill="#444")
# サンプル液（光路長0.5mm）
d.rectangle([cvx-13, cvy-29, cvx+13, cvy-28], outline="#c00", width=1, fill="#ffe8e8")
d.text((cvx, cvy-28), text="サンプル液\n(光路長 0.5mm)", anchor="mm", font=font(7), fill="#c00")
# 316L ワッシャー（下）
d.rectangle([cvx-35, cvy-28, cvx+35, cvy-27], outline="#444", width=2, fill="#ccc")
# UV-grade石英窓（下）
d.rectangle([cvx-35, cvy-26, cvx+35, cvy-16], outline="#0af", width=2, fill="#e0f4ff")
d.text((cvx, cvy-10), text="UV-grade fused silica 窓", anchor="mm", font=font(8), fill="#0af")

# 検出器 SG01S-C18 + TIA — キュベットの反対側（光軸の延長上）
detx, dety = 490, 680
d.rectangle([detx-55, dety-22, detx+55, dety+22], outline="#000", width=2, fill="#fff")
d.text((detx, dety), text="SG01S-C18\n+ TIA", anchor="mm", font=font(11), fill="#000")

# 光軸（サンプル→検出器）
d.line([cvx, cvy+30, detx, dety-22], fill="#00c", width=2)
d.text(((cvx+detx)/2+70, (cvy+dety)/2), text="光軸", anchor="mm", font=font(10), fill="#00c")

# LED 265nm — 左側、右下（45°）へ内傾
lx, ly = 300, 470
d.rectangle([lx-45, ly-28, lx+45, ly+28], outline="#c00", width=2, fill="#ffe8e8")
d.text((lx, ly), text="UVC LED\n265nm\n(内傾 ~45°)", anchor="mm", font=font(11), fill="#c00")

# LED 280nm — 右側、左下（45°）へ内傾
rx, ry = 680, 470
d.rectangle([rx-45, ry-28, rx+45, ry+28], outline="#0a0", width=2, fill="#e8ffe8")
d.text((rx, ry), text="UVC LED\n280nm\n(内傾 ~45°)", anchor="mm", font=font(11), fill="#0a0")

# 各LEDからキュベット中心へのビーム（V字）
d.line([lx+30, ly+22, cvx-15, cvy-18], fill="#c00", width=2)
d.text(((lx+rx)/2 - 140, ly+70), text="265nm ビーム", anchor="mm", font=font(9), fill="#c00")
d.line([rx-30, ry+22, cvx+15, cvy-18], fill="#0a0", width=2)
d.text(((lx+rx)/2 + 140, ly+70), text="280nm ビーム", anchor="mm", font=font(9), fill="#0a0")

# 交差点（キュベット中心）に星印
d.arc((cvx-8, cvy-8, cvx+8, cvy+8), start=0, end=360, fill="#f00", width=2)

# V字の角度注記
d.text((lx+95, ly+100), text="θ≈45°", anchor="mm", font=font(10), fill="#777")
d.text((rx-95, ry+100), text="θ≈45°", anchor="mm", font=font(10), fill="#777")

# 放熱板干渉なし注記
d.text((490, 382), text="V字配置 → 2LEDの放熱板が干渉せず、切断不要。内傾でビームをキュベット中心に集光。", anchor="mm", font=font(10), fill="#555")

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "nanodrop_circuit.png")
img.save(out)
print("saved", out, img.size)
